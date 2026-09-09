#ifndef JOURNAL_H
#define JOURNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__cplusplus)
    #define STATIC_ASSERT static_assert
#else
    #define STATIC_ASSERT _Static_assert
#endif

#include "storage.h"
#include "mem_layout.h"


#define JRNL_SECTOR_SIZE 3

#define JRNL_HEADER_DATA_SIZE 8
// Journal magic number to check header corruption
//#define JRNL_MAGIC 0x4A524E4Cu
#define JRNL_MAGIC 0x4A524E5Cu
#define JOURNAL_PADDING SECTOR_SIZE - sizeof(JournalHeaderData)  - sizeof(uint32_t) * 3


typedef uint8_t JRNL_STATE;
typedef uint8_t JRNL_TYPE;
typedef uint8_t JRNL_HEAD_STATUS;

// Journal States
enum {
    JRNL_EMPTY = 0,
    JRNL_ACTIVE,
    JRNL_COMMITTED,
};

// Jounral Header Status
enum {
    JRNL_READ_ERROR = 0,
    JRNL_UNINITIALIZED,
    JRNL_CORRUPTED,
    JRNL_VALID,
    JRNL_ROLLBACK,
};

// Stored Sector Type
enum {
    JRNL_CONTACT = 0,
    JRNL_MESSAGE
};



// Journal Header Data (8B)
typedef struct {
    uint32_t magic; // magic number to check header validation (4B)
    JRNL_STATE state; // Journal State (For power cycle rollback) (1B)
    JRNL_TYPE type; // Type of sector stored in journal (1B)
    uint16_t sector; // Sector index (2B)
} JournalHeaderData;

// JournalHeaderData Buffer Union (8B)
typedef union {
    JournalHeaderData var;
    uint8_t buffer[sizeof(JournalHeaderData)];
} JournalHeaderDataB;

// Journal Header (512B, needs to be 512B because will be read from SD card)
typedef struct {
    JournalHeaderDataB data;
    uint32_t header_crc; // Journal header data CRC-32 (4B)
    uint32_t content_crc; // Journal Content CRC-32 (4B)
    uint32_t usage_bitmap_crc; // Journal Usage Bitmap CRC-32 (4B)
    uint8_t padding[JOURNAL_PADDING];
} JournalHeader;


typedef union {
    JournalHeader var;
    uint8_t buffer[sizeof(JournalHeader)];
} JournalHeaderBuffer;

// Rollback Journal Struct (Creating this way for formard declaration in other files)
typedef struct Journal {
    Storage *storage; // Storage struct pointer
    JournalHeaderBuffer header; // Journal Header (allocated memory to read sd card mem into)
    uint8_t content[SECTOR_SIZE]; // Journal Sector (allocated memory to read sd card mem into) 
    uint8_t usage_bitmap_sector[SECTOR_SIZE]; // Journal Bitmap
} Journal;

// Static check JournalHeader size
STATIC_ASSERT(sizeof(JournalHeaderData) == JRNL_HEADER_DATA_SIZE, "Unexpected JournalHeaderData\
        size");
STATIC_ASSERT(sizeof(JournalHeader) == SECTOR_SIZE, "Unexpected JournalHeader size");

/**
 * @brief Initialise the database journal
 *
 * @param journal journal struct pointer (allocated in database struct)
 * @param storage storage abstraction struct pointer (allocated in database struct) 
 *
 * @retval true Journal Init successful 
 * @retval false journal init fail 
 */
bool journal_init(Journal *journal, Storage *storage);

void journal_data_init(JournalHeaderDataB *jData, JRNL_TYPE type, uint16_t sectorInd);

bool journal_add(Journal *journal, JRNL_TYPE type, uint16_t index, uint8_t *content);

bool journal_free(Journal *journal);

#if defined (HOST_BUILD)

/**
 * @brief Initialise header and write to storage
 *
 * @param journal journal struct pointer (allocated in database struct)
 *
 * @retval true Journal init header write successful
 * @retval false journal init header write fail 
 */
bool journal_header_init(Journal* journal);

/**
 * @brief Write to the journal
 *
 * @param journal journal struct pointer (allocated in database struct)
 * @param header journal header being written
 * @param content old content being written to journel incase of rollback 
 *
 * @retval true journal write successful
 * @retval false journal write fail 
 */
bool journal_write(Journal *journal, JournalHeaderBuffer* header, uint8_t *content, uint8_t
        *usage_bitmap);

bool journal_rollback(Journal *journal);

/**
 * @brief Write to the journal
 *
 * @param journal journal struct pointer (allocated in database struct)
 *
 * @retval true journal read successful
 * @retval false journal read fail 
 */
bool journal_header_read(Journal *journal, JournalHeaderBuffer *out);

/**
 * @brief Write to the journal
 *
 * @param journal journal struct pointer (allocated in database struct)
 *
 * @retval true journal read successful
 * @retval false journal read fail 
 */
bool journal_content_read(Journal *journal);

/**
 * @brief Read journal bitmap from SD Card
 *
 * @param journal journal struct pointer (allocated in database struct)
 *
 * @retval true journal read successful
 * @retval false journal read fail 
 */
bool journal_usage_read(Journal *journal);

/**
 * @brief Check if the journal status
 *
 * @param journal journal struct pointer (allocated in database struct)
 *
 * @retval status of the journal header and if the database is in need of rollback
 */
JRNL_HEAD_STATUS get_journal_status(Journal* journal);
#endif

#ifdef __cplusplus
}
#endif

#endif
