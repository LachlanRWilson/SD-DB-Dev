#include "journal.h"
#include "usage_bitmap.h"


// Forward Declaration
JRNL_HEAD_STATUS get_journal_status(Journal* journal);
bool journal_header_init(Journal* journal);
bool journal_rollback(Journal *journal);
STRG_RET journal_free(Journal *journal);


STRG_RET journal_read_sector(Journal *journal, uint16_t journal_section, uint8_t *out)
{
    return read_sector(journal->storage, journal_section, out);
}

STRG_RET journal_write_sector(Journal *journal, uint16_t journal_section, uint8_t *in)
{
    return write_sector(journal->storage, journal_section, in);
}

/**
 * @brief Initialise the database journal
 *
 * @param journal journal struct pointer (allocated in database struct)
 * @param storage storage abstraction struct pointer (allocated in database struct)
 *
 * @retval true Journal Init successful
 * @retval false journal init fail
 */
bool journal_init(Journal *journal, Storage *storage)
{
    journal->storage = storage;

    // read the journal from the sd card and determine the status of the journal
    switch(get_journal_status(journal)) {


        case JRNL_CORRUPTED: // Storage read error
        case JRNL_READ_ERROR: // GAME OVER (Database fucked)
            return false;

        // journal header not init
        case JRNL_UNINITIALIZED:
            return journal_header_init(journal);

        case JRNL_ROLLBACK:
            return journal_rollback(journal);

        case JRNL_VALID:
            return true;
        default:
            // unkown return from get_journal_status
            return false;
    };


}


/**
 * @brief Initialise header and write to storage
 *
 * @param journal journal struct pointer (allocated in database struct)
 *
 * @retval true Journal init header write successful
 * @retval false journal init header write fail
 */
bool journal_header_init(Journal* journal)
{
    // init header buffer to write
    JournalHeaderBuffer jHeadBuff = {
        .var = {
            .data = {
                .var = {
                    .magic = JRNL_MAGIC,
                    .state = JRNL_EMPTY,
                    .type = 0,
                    .sector = 0
                },
            },
        },
    };

    return journal_write_sector(journal, JRNL_HEADER_SECTOR, jHeadBuff.buffer);

}

/**
 * @brief Write to the journal on the SD Card
 *
 * @param journal journal struct pointer (allocated in database struct)
 * @param header journal header being written
 * @param content old content being written to journel incase of rollback
 *
 * @retval true journal write successful
 * @retval false journal write fail
 */
STRG_RET journal_write(Journal *journal, JournalHeaderBuffer* header, uint8_t *content, uint8_t
        *usage_bitmap)
{
    STRG_RET ret;

    // Write usage bitmap if given
    if (usage_bitmap != NULL)
    {
        ret = journal_write_sector(journal, JRNL_USAGE_SECTOR, usage_bitmap);
        if (ret != STRG_OK)
        {
            return ret;
        }
    }

    // Write content
    ret = journal_write_sector(journal, JRNL_CONTENT_SECTOR, content);
    if (ret != STRG_OK)
    {
        return ret;
    }

    // Write header
    return journal_write_sector(journal, JRNL_HEADER_SECTOR, header->buffer);
}

void journal_data_init(JournalHeaderDataB *jData, JRNL_TYPE type, uint16_t sectorInd)
{
    jData->var.magic = JRNL_MAGIC;
    jData->var.state = JRNL_ACTIVE;
    jData->var.type = type;
    jData->var.sector = sectorInd;

}

/**
 * @brief Add sector and usage map to the back up journal. This is done before any changes are made
 * to it in RAM. Allowing Database rollback if write failure.
 *
 * @param journal journal struct pointer (allocated in database struct)
 * @param type type of sector the index is pointing at (NOTE: usage_bitmap_sector is only applicable for JRNL_CONTACT
 * and JRNL_MESSAGE types)
 * @param content old content being written to journel incase of rollback
 * @param index raw sector index of the content that is being journalled
 *
 * @retval true journal add successful
 * @retval false journal add fail
 */
bool journal_add(Journal *journal, JRNL_TYPE type, uint16_t index, uint8_t *content)
{

    // Init Header
    JournalHeaderDataB jData;
    uint32_t* usage_bitmap_sector = NULL;
    journal_data_init(&jData, type, index);

    // only add usage bitmap if contact or message data (usage bitmap tracks
    // said data unlike call / message history which uses ring buffers)
    if (type == JRNL_CONTACT || type == JRNL_MESSAGE)
    {
        // Get the sector in RAM of the usage bitmap vector
        usage_bitmap_sector = &usage_bitmap[USAGE_BITMAP_FIND_SECTOR(index) *
            ELEMENTS_PER_SECTOR];
    }

    // Create the journal header
    JournalHeaderBuffer jHeadBuff = {
        .var = {
            .data = jData,
        }
    };

    // Keep the active header in RAM so journal_free() commits this entry
    journal->header = jHeadBuff;

    // Write to the journal
    return journal_write(journal, &jHeadBuff, content, (uint8_t*)usage_bitmap_sector) == STRG_OK;

}

/**
 * @brief Take the journal header information and return the database to the previously journelled state.
 *
 * Note:
 * Only contact and message data rollbacks the usage bitmap. Both call and message history do not require a usage
 * bitmap as they are managed by a ring buffer
 *
 * @param journal journal struct pointer (allocated in database struct)
 *
 * @retval true journal rollback successful
 * @retval false journal rollback fail due to storage error
 */
bool journal_rollback(Journal *journal)
{
    JournalHeader *header = &journal->header.var;
    Storage *storage = journal->storage;
    STRG_RET ret;

    ret = journal_read_sector(journal, JRNL_CONTENT_SECTOR, journal->content);

    if (ret != STRG_OK)
    {
        return false;
    }

    // write journal content back to sd card (TODO: SDMMC callback for write confirmation)
    ret = write_sector(storage, header->data.var.sector + DATA_REGION_START_SECTOR, journal->content);
    if (ret != STRG_OK)
    {
        return false;
    }

    // If rollback on call history or message history do not concern with usage bitmap rollback
    if (header->data.var.type != JRNL_CONTACT && header->data.var.type != JRNL_MESSAGE)
    {
        return journal_free(journal) == STRG_OK;
    }

    ret = journal_read_sector(journal, JRNL_USAGE_SECTOR, journal->usage_bitmap_sector);

    if (ret != STRG_OK)
    {
        return false;
    }


    // write jounral usage bitmap back to sd card
    ret = write_sector(storage, USAGE_BITMAP_START_SECTOR + USAGE_BITMAP_FIND_SECTOR(header->data.var.sector),
                       journal->usage_bitmap_sector);

    if (ret != STRG_OK)
    {
        return false;
    }

    // free the journal
    return journal_free(journal) == STRG_OK;
}


/**
 * @brief Check if the journal status
 *
 * @param journal journal struct pointer (allocated in database struct)
 *
 * @retval status of the journal header and if the database is in need of rollback
 */
JRNL_HEAD_STATUS get_journal_status(Journal* journal)
{
    JournalHeaderBuffer jHeadBuff;

    // Read journal header from sd card
    STRG_RET ret = read_sector_raw(journal->storage, JRNL_HEADER_SECTOR, jHeadBuff.buffer);
    if (ret != STRG_OK)
    {
        return JRNL_READ_ERROR;
    }

    uint32_t magic = jHeadBuff.var.data.var.magic;

    if (!sector_crc_valid(jHeadBuff.buffer))
    {
        // check if magic is uninit
        if (magic == JRNL_HEADER_MAGIC_EMPTY || magic == JRNL_HEADER_MAGIC_FULL)
        {
            return JRNL_UNINITIALIZED;
        }

        return JRNL_CORRUPTED;
    }

    // Check the magic number to see if initialised
    if (magic != JRNL_MAGIC) {
        return JRNL_UNINITIALIZED;
    }

    // No issues with header, store in struct
    journal->header = jHeadBuff;

    // Check if the journal is active
    if (jHeadBuff.var.data.var.state == JRNL_ACTIVE)
    {
        return JRNL_ROLLBACK;
    }

    return JRNL_VALID;
}


/**
 * @brief set the journal to free state
 *
 * @param journal journal struct pointer (allocated in database struct)
 *
 * @retval True if journal state updated
 * @retval Flase if journal state update fail
 */
STRG_RET journal_free(Journal *journal)
{
    Storage *storage = journal->storage;

    // Set journal header to committed
    journal->header.var.data.var.state = JRNL_COMMITTED;

    // Write update to SD card
    return journal_write_sector(journal, JRNL_HEADER_SECTOR, journal->header.buffer);
}

