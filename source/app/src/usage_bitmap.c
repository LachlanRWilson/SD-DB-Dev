#include "usage_bitmap.h"
#include "iterator.h"
#include <string.h>

#ifndef HOST_BUILD
// Read entire usage bitmap into RAM (5.6KB)
__attribute__((section(".ram_d1")))
uint32_t usage_bitmap[USAGE_BITMAP_STORAGE_SIZE];
#else
uint32_t usage_bitmap[USAGE_BITMAP_STORAGE_SIZE];
#endif



/**
 * @brief Write zero-initialised bitmap to storage
 *
 * @param storage storage access struct
 * @retval True if successful write else false.
 */
bool init_usage_bitmap(Storage *storage)
{
    // Zero-set entire bitmap
    memset(usage_bitmap, 0, sizeof(usage_bitmap));

    // Stamp the CRC trailer of every bitmap sector
    for (uint32_t sector = 0; sector < USAGE_BITMAP_SECTOR_SIZE; sector++)
    {
        sector_crc_stamp((uint8_t *)&usage_bitmap[sector * ELEMENTS_PER_SECTOR]);
    }

    return storage->write_multiblock(storage->context, USAGE_BITMAP_START_SECTOR,
            USAGE_BITMAP_SECTOR_SIZE, (uint8_t *) usage_bitmap);
}

/**
 * @brief Read the usage bitmap to RAM
 *
 * @param storage storage access struct
 * @param out read out usage bitmap sector
 * @retval True if successful read and every sector CRC is valid else false.
 */
bool read_usage_bitmap(Storage* storage)
{
    if (!storage->read_multiblock(storage->context, USAGE_BITMAP_START_SECTOR,
            USAGE_BITMAP_SECTOR_SIZE, (uint8_t *)usage_bitmap))
    {
        return false;
    }

    // Check the CRC trailer of every bitmap sector
    for (uint32_t sector = 0; sector < USAGE_BITMAP_SECTOR_SIZE; sector++)
    {
        if (!sector_crc_valid((uint8_t *)&usage_bitmap[sector * ELEMENTS_PER_SECTOR]))
        {
            return false;
        }
    }

    return true;
}


/**
 * @brief Check the usage bit in RAM
 *
 * @param index
 * @retval True if used else false
 */
bool check_usage_bit(uint16_t index)
{
    return (usage_bitmap[USAGE_BITMAP_FIND_INDEX(index)] >> USAGE_BITMAP_FIND_BIT(index)) & 0x1;
}


/**
 * @brief Do a blind write to usage bitmap based on in RAM bitmap
 *
 * @param index
 * @retval True is successful write, else false
 */
STRG_RET update_usage_bit(Storage *storage, uint16_t index, bool used_state)
{
    // Bit map sector
    uint32_t bitmap_sector = USAGE_BITMAP_FIND_SECTOR(index);

    // Sector start pointer
    uint32_t *write_sector = &usage_bitmap[bitmap_sector * ELEMENTS_PER_SECTOR];

    // uint32_t element index in sector
    uint32_t element = USAGE_BITMAP_FIND_ELEMENT(index);

    // bit in uint32_t element
    uint32_t bit = USAGE_BITMAP_FIND_BIT(index);

    // check if the bit has actually changed
    bool prev_bit_val = ((write_sector[element]) & (uint32_t)(1U << bit)) ? true : false;

    if (used_state) {
        // Set bit
        write_sector[element] |= (1U << bit);
    } else {
        // Unset bit
        write_sector[element] &= ~(1U << bit);
    }

    // Update the sector CRC trailer in RAM so it matches what is written
    sector_crc_stamp((uint8_t *)write_sector);

    STRG_RET ret = storage->write_block( storage->context, USAGE_BITMAP_START_SECTOR + bitmap_sector,
            (uint8_t *)write_sector);

    // If the write fails, return the bit back to what it was before
    if (ret != STRG_OK)
    {
        // if the bit has changed, change back
        if (prev_bit_val != used_state)
        {
            if (!used_state) {
                // Set bit
                write_sector[element] |= (1U << bit);
            } else {
                // Unset bit
                write_sector[element] &= ~(1U << bit);
            }
            sector_crc_stamp((uint8_t *)write_sector);
        }
        return ret;
    }
    return STRG_OK;
}

/**
 * @brief return the index of the next bit set in the bitmap
 *
 * @param curInd the current bit index
 * @retval UINT16_MAX if fail, else the bit index
 */
uint16_t get_next_bit(uint16_t curInd, uint16_t limInd)
{
    // The starting word for message sector
    int first_usage_elem = USAGE_BITMAP_FIND_INDEX(curInd);
    int first_usage_bit = USAGE_BITMAP_FIND_BIT(curInd);

    // Get the last uint32_t which stores a message sector usage bit
    int last_usage_elem = USAGE_BITMAP_FIND_INDEX(limInd);



    for (uint32_t word = first_usage_elem; word <= last_usage_elem; word++)
    {
        // skip sector CRC trailers
        if (USAGE_BITMAP_IS_CRC_WORD(word))
        {
            continue;
        }

        uint32_t bits = usage_bitmap[word];

        // if the first bit of the work is not a message, clear bits that are not messages
        if ((word == first_usage_elem) && (first_usage_bit > 0))
        {
            clear_bits_to_n_u32(&bits, first_usage_bit);
        }

        while (bits != 0)
        {
            uint32_t bit = __builtin_ctz(bits);
            return (uint16_t)(USAGE_BITMAP_WORD_TO_INDEX(word) + bit);
        }
    }
    return UINT16_MAX;
}

/**
 * @brief return the index of the next bit set in the bitmap
 *
 * @param curInd the current bit index
 * @retval UINT16_MAX if fail, else the bit index
 */
uint16_t get_prev_bit(uint16_t curInd, uint16_t limInd)
{
    int first_usage_elem = USAGE_BITMAP_FIND_INDEX(limInd);
    int first_usage_bit = USAGE_BITMAP_FIND_BIT(limInd);

    int last_usage_elem = USAGE_BITMAP_FIND_INDEX(curInd);
    int last_usage_bit = USAGE_BITMAP_FIND_BIT(curInd);

    for (int word = first_usage_elem; word >= last_usage_elem; word--)
    {
        // skip sector CRC trailers
        if (USAGE_BITMAP_IS_CRC_WORD(word))
        {
            continue;
        }

        uint32_t bits = usage_bitmap[word];

        // For the first word, only consider bits <= first_usage_bit
        // (when first_usage_bit is the word's top bit, every bit already qualifies,
        // and shifting a uint32_t left by 32 is undefined behaviour)
        if (word == first_usage_elem && first_usage_bit < (int)BITS_PER_ELEMENT - 1)
        {
            bits &= (1u << (first_usage_bit + 1)) - 1;
        }

        // For the last word, only consider bits >= last_usage_bit
        if (word == last_usage_elem && last_usage_bit > 0)
        {
            bits &= ~((1u << last_usage_bit) - 1);
        }

        if (bits != 0)
        {
            uint32_t bit = 31u - __builtin_clz(bits);

            return (uint16_t)(USAGE_BITMAP_WORD_TO_INDEX(word) + bit);
        }

    }
    return UINT16_MAX;
}


/**
 * @brief Find the nth set bit in a bitmap
 *
 * @param bitmap bitmap being searched
 * @param n the nth bit to find
 * @retval True is successful write, else false
 */
uint16_t get_nth_set_bit(uint32_t bitmap, int n)
{
    while (bitmap) {
        int bit = __builtin_ctz(bitmap);

        if (n == 0)
            return bit;

        bitmap &= bitmap - 1;
        n--;
    }

    return UINT16_MAX;
}


/* Iterator Funtions */

/**
 * @brief Find the nth set bit in a bitmap
 *
 * @param bitmap bitmap being searched
 * @param n the nth bit to find
 * @retval True is successful write, else false
 */
bool usage_bitmap_iterator_get(Iterator *it, void* out)
{
    if (it == NULL || out == NULL)
    {
        return false;
    }

    // Dereference Context
    UsageBitmapIteratorCtx ctx = *(UsageBitmapIteratorCtx*) it->context;

    // No current position (iteration not started, or exhausted)
    if (ctx.currBit == UINT16_MAX)
    {
        return false;
    }

    *(uint16_t *)out = ctx.currBit;
    return true;
}

/**
 * @brief Find the nth set bit in a bitmap
 *
 * @param bitmap bitmap being searched
 * @param n the nth bit to find
 * @retval True is successful write, else false
 */
bool usage_bitmap_iterator_next(Iterator *it)
{
    if (it == NULL)
    {
        return false;
    }

    UsageBitmapIteratorCtx *ctx = (UsageBitmapIteratorCtx*) it->context;

    if (ctx->total_bits == 0)
    {
        ctx->currBit = UINT16_MAX;
        return false;
    }

    // Search strictly after the current position, or from the start if not yet positioned
    uint16_t start = (ctx->currBit == UINT16_MAX) ? 0 : (uint16_t)(ctx->currBit + 1);

    if (start >= ctx->total_bits)
    {
        ctx->currBit = UINT16_MAX;
        return false;
    }

    uint16_t found = get_next_bit(start, (uint16_t)(ctx->total_bits - 1));

    ctx->currBit = found;
    return found != UINT16_MAX;
}

/**
 * @brief Find the nth set bit in a bitmap
 *
 * @param bitmap bitmap being searched
 * @param n the nth bit to find
 * @retval True is successful write, else false
 */
bool usage_bitmap_iterator_prev(Iterator *it)
{
    if (it == NULL)
    {
        return false;
    }

    UsageBitmapIteratorCtx *ctx = (UsageBitmapIteratorCtx*) it->context;

    if (ctx->total_bits == 0)
    {
        ctx->currBit = UINT16_MAX;
        return false;
    }

    // Search strictly before the current position, or from the end if not yet positioned
    if (ctx->currBit == 0)
    {
        ctx->currBit = UINT16_MAX;
        return false;
    }

    uint16_t end = (ctx->currBit == UINT16_MAX) ? (uint16_t)(ctx->total_bits - 1) : (uint16_t)(ctx->currBit - 1);

    uint16_t found = get_prev_bit(0, end);

    ctx->currBit = found;
    return found != UINT16_MAX;
}

/**
 * @brief Initialise a Usage bitmap iterator with appropriate context and function pointers
 *
 * @param bitmap bitmap being searched
 * @param n the nth bit to find
 * @retval True is successful write, else false
 */
Iterator usage_bitmap_iterator_init(UsageBitmapIteratorCtx *it_ctx,uint16_t total_bits, uint16_t total_elems)
{
    it_ctx->currBit = UINT16_MAX;
    it_ctx->total_bits = total_bits;
    it_ctx->total_elems = total_elems;

    Iterator it = {0};
    it.context = (void *)it_ctx;
    it.next = usage_bitmap_iterator_next;
    it.prev = usage_bitmap_iterator_prev;
    it.get = usage_bitmap_iterator_get;
    return it;

}
