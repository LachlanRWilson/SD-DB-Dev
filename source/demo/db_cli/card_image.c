#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "card_image.h"
#include "heap_storage.h"

// database.c backing storage for the host build (not declared in a header)
extern uint8_t heap_mem[];

// heap_storage.c exports these but heap_storage.h only declares the single block calls
bool HeapStorage_ReadMultiBlock(void *context, uint32_t index, size_t readNum, uint8_t *out);
bool HeapStorage_WriteMultiBlock(void *context, uint32_t index, size_t writeNum, uint8_t *in);

CardStats card_stats;

static int g_fd = -1;
static int g_writes_left = -1; // -1 = never fail

/**
 * @brief Write sectors from heap storage through to the image file
 *
 * @param index first raw sector
 * @param count number of sectors
 * @retval true if written
 */
static bool image_write(uint32_t index, size_t count)
{
    size_t bytes = count * SECTOR_SIZE;
    off_t offset = (off_t)index * SECTOR_SIZE;

    return pwrite(g_fd, &heap_mem[offset], bytes, offset) == (ssize_t)bytes;
}

/**
 * @brief Take one sector write off the power cut budget
 *
 * @retval true if the write may go ahead
 */
static bool power_on(void)
{
    if (g_writes_left == 0)
    {
        card_stats.failed++;
        return false;
    }
    if (g_writes_left > 0)
    {
        g_writes_left--;
    }
    return true;
}

static bool Card_ReadBlock(void *context, uint32_t index, uint8_t *out)
{
    card_stats.reads++;
    return HeapStorage_ReadBlock(context, index, out);
}

static bool Card_ReadMultiBlock(void *context, uint32_t index, size_t readNum, uint8_t *out)
{
    card_stats.reads += (uint32_t)readNum;
    return HeapStorage_ReadMultiBlock(context, index, readNum, out);
}

static bool Card_WriteBlock(void *context, uint32_t index, uint8_t *in)
{
    if (!power_on())
    {
        return false;
    }

    card_stats.writes++;
    return HeapStorage_WriteBlock(context, index, in) && image_write(index, 1);
}

static bool Card_WriteMultiBlock(void *context, uint32_t index, size_t writeNum, uint8_t *in)
{
    // one sector at a time so a power cut can land part way through
    for (size_t i = 0; i < writeNum; i++)
    {
        if (!Card_WriteBlock(context, index + (uint32_t)i, in + (i * SECTOR_SIZE)))
        {
            return false;
        }
    }
    return true;
}

bool card_image_open(const char *path)
{
    struct stat st;
    ssize_t got;

    g_fd = open(path, O_RDWR | O_CREAT, 0644);
    if (g_fd < 0)
    {
        perror(path);
        return false;
    }

    // a missing or short image reads as a blank (zeroed) card
    memset(heap_mem, 0, CARD_BYTES);
    if ((fstat(g_fd, &st) == 0) && (st.st_size > 0))
    {
        size_t want = ((size_t)st.st_size < CARD_BYTES) ? (size_t)st.st_size : CARD_BYTES;
        got = pread(g_fd, heap_mem, want, 0);
        if (got != (ssize_t)want)
        {
            perror("read image");
            return false;
        }
    }

    return ftruncate(g_fd, (off_t)CARD_BYTES) == 0;
}

bool card_image_flush(void)
{
    return (g_fd >= 0) && image_write(0, CARD_RAW_SECTORS) && (fsync(g_fd) == 0);
}

void card_image_close(void)
{
    if (g_fd >= 0)
    {
        fsync(g_fd);
        close(g_fd);
        g_fd = -1;
    }
}

void card_image_wrap(Database *db)
{
    db->storage.read_block = Card_ReadBlock;
    db->storage.read_multiblock = Card_ReadMultiBlock;
    db->storage.write_block = Card_WriteBlock;
    db->storage.write_multiblock = Card_WriteMultiBlock;
}

void card_image_cut_after(int writes)
{
    g_writes_left = writes;
}
