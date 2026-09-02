#include "journal.h"
#include "usage_bitmap.h"
#include "crc.h"


// Forward Declaration
JRNL_HEAD_STATUS get_journal_status(Journal* journal);
bool journal_header_init(Journal* journal);
bool journal_rollback(Journal *journal);
bool journal_free(Journal *journal);

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

    // calculate the crc on the journal header data only
    jHeadBuff.var.header_crc = crc32_calculate(jHeadBuff.var.data.buffer,
            sizeof(JournalHeaderData));

    // write journal header to sd card
    Storage *storage = journal->storage;
    return storage->write_block(storage->context, JRNL_HEADER_SECTOR, jHeadBuff.buffer);

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
bool journal_write(Journal *journal, JournalHeaderBuffer* header, uint8_t *content, uint8_t
        *usage_bitmap)
{
    // allocate pointer to make it more readable
    Storage *storage = journal->storage;

    // Write header
    if (!storage->write_block(storage->context, JRNL_HEADER_SECTOR, header->buffer))
    {
        return false;
    }

    // Write content
    if (!storage->write_block(storage->context, JRNL_CONTENT_SECTOR, content))
    {
        return false;
    }

    return storage->write_block(storage->context, JRNL_USAGE_SECTOR, usage_bitmap);
}

bool journal_data_init(JournalHeaderDataB *jData, JRNL_TYPE type, uint16_t sectorInd)
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
 * @param type type of sector the index is pointing at 
 * @param content old content being written to journel incase of rollback 
 * @param index sector index of the content that is being journalled
 *
 * @retval true journal add successful
 * @retval false journal add fail 
 */
bool journal_add(Journal *journal, JRNL_TYPE type, uint16_t index, uint8_t *content)
{

    // Init Header
    JournalHeaderDataB jData;
    journal_data_init(&jData, type, index);

    // Calc header CRC
    uint32_t header_crc = crc32_calculate(jData.buffer, sizeof(JournalHeaderData));
    uint32_t content_crc = crc32_calculate(content, SECTOR_SIZE);

    // Get the sector in RAM of the usage bitmap vector
    uint32_t* usage_bitmap_sector = &usage_bitmap[USAGE_BITMAP_FIND_SECTOR(index) *
        ELEMENTS_PER_SECTOR];

    // Get usage bitmap sector CRC
    uint32_t usage_bitmap_crc = crc32_calculate((uint8_t*)usage_bitmap_sector, SECTOR_SIZE);
    
    // Create the journal header
    JournalHeaderBuffer jHeadBuff = {
        .var = {
            .data = jData,
            .header_crc = header_crc,
            .content_crc = content_crc,
            .usage_bitmap_crc = usage_bitmap_crc
        }
    };

    // Write to the journal
    return journal_write(journal, &jHeadBuff, content, (uint8_t*)usage_bitmap_sector);

}

/**
 * @brief Read journal header from the SD Card
 *
 * @param journal journal struct pointer (allocated in database struct)
 *
 * @retval true journal read successful
 * @retval false journal read fail 
 */
bool journal_header_read(Journal *journal, JournalHeaderBuffer *out)
{
    // Read journel sector
    return journal->storage->read_block(journal->storage->context, JRNL_HEADER_SECTOR, out->buffer);
}

/**
 * @brief Read journal bitmap from SD Card
 *
 * @param journal journal struct pointer (allocated in database struct)
 *
 * @retval true journal read successful
 * @retval false journal read fail 
 */
bool journal_usage_read(Journal *journal)
{

    return journal->storage->read_block(journal->storage->context, JRNL_USAGE_SECTOR,
            journal->usage_bitmap_sector);
}

/**
 * @brief Write to the journal
 *
 * @param journal journal struct pointer (allocated in database struct)
 *
 * @retval true journal read successful
 * @retval false journal read fail 
 */
bool journal_content_read(Journal *journal)
{
    // Read journel sector
    return journal->storage->read_block(journal->storage->context, JRNL_CONTENT_SECTOR,
            journal->content);

}

bool journal_rollback(Journal *journal)
{
    JournalHeader header = journal->header.var;
    Storage *storage = journal->storage;

    if (!journal_content_read(journal) || !journal_usage_read(journal))
    {
        return false;
    }

    // Check content crc (header crc already checked)
    uint32_t content_crc = crc32_calculate(journal->content, SECTOR_SIZE);
    uint32_t usage_crc = crc32_calculate(journal->usage_bitmap_sector, SECTOR_SIZE);

    if (content_crc != header.content_crc || usage_crc != header.usage_bitmap_crc)
    {
        return false; // Journal corruption (PANIC)
    }


    // write journal content back to sd card (TODO: SDMMC callback for write confirmation)
    if (!storage->write_block(storage->context, header.data.var.sector, journal->content))
    {
        return false;
    }

    // write jounral usage bitmap back to sd card
    if (!storage->write_block(storage->context, USAGE_BITMAP_START_SECTOR +
                USAGE_BITMAP_FIND_SECTOR(header.data.var.sector), journal->usage_bitmap_sector))
    {
        return false;
    }
    
    // free the journal 
    return journal_free(journal);
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
    if (!journal_header_read(journal, &jHeadBuff))
    {
        return JRNL_READ_ERROR;
    }

    // Check the magic number to see if initialised
    if (jHeadBuff.var.data.var.magic != JRNL_MAGIC) {
        return JRNL_UNINITIALIZED;
    }

    // Calc CRC
    uint32_t crc = crc32_calculate(jHeadBuff.var.data.buffer,
            sizeof(JournalHeaderData));

    // Check calculated crc with stored crc
    if (crc != jHeadBuff.var.header_crc)
    {
        return JRNL_CORRUPTED;
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
bool journal_free(Journal *journal)
{
    Storage *storage = journal->storage;

    // Set journal header to committed
    journal->header.var.data.var.state = JRNL_COMMITTED;

    // Write update to SD card
    return storage->write_block(storage->context, JRNL_HEADER_SECTOR, journal->header.buffer);
}
