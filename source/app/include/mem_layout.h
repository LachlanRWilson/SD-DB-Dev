#ifndef MEM_LAYOUT_H
#define MEM_LAYOUT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* ================================================================
 * Fundamental storage constants
 * ================================================================ */
#define SECTOR_SIZE 512

// Every data sector ends in a CRC-32 trailer covering bytes [0, SECTOR_PAYLOAD_BYTES)
#define SECTOR_CRC_BYTES 4
#define SECTOR_PAYLOAD_BYTES (SECTOR_SIZE - SECTOR_CRC_BYTES)

#define SECTORS_REQUIRED(bytes) \
    (((bytes) + SECTOR_SIZE - 1) / SECTOR_SIZE)
;

/* ================================================================
 * Database sizing knobs — single source of truth
 * ================================================================
 * These used to be scattered across hash_table.h / contact.h /
 * usage_bitmap.h, each including the others just to compute a byte
 * count or sector offset. That's what caused the circular includes.
 *
 * Rule going forward: if a value is a *number* used for layout math
 * (capacities, sector counts, sector start offsets), it lives here.
 * If it's a *struct definition* (Contact, HashEntry, MessageBlock),
 * it stays in its own header. Struct-size STATIC_ASSERTs in those
 * headers keep the numbers below honest.
 */

// Number of contacts / hash table slots the system supports
#define HASH_TABLE_SIZE 14293
#define HASH_TABLE_ENTRIES 10000

// Raw byte size of one on-disk Contact record.
// Kept in sync with sizeof(Contact) via STATIC_ASSERT in contact.h.
#define CONTACT_RECORD_BYTES 81

// Byte size of ContactSectorHeader (the small used-bitmap header at
// the front of every ContactSector). Checked via STATIC_ASSERT in
// contact.h.
#define CONTACT_SECTOR_HEADER_BYTES 1

// Byte size of the HashEntry struct (see hash_table.h). Only needed
// here for layout math — the struct itself still lives in hash_table.h.
#define HASH_ENTRY_BYTES 8

// Byte size of the
#define MESSAGE_HISTORY_SECTOR_BYTES SECTOR_SIZE

/* ---- Contact sector layout ------------------------------------- */

// How many Contact records fit in one 512B sector, after the sector
// type tag, the ContactSectorHeader and the CRC trailer.
#define CONTACT_SECTOR_CAPACITY \
    ((SECTOR_PAYLOAD_BYTES - CONTACT_SECTOR_HEADER_BYTES - sizeof(SECTOR_TYPE)) / CONTACT_RECORD_BYTES)

#define CONTACT_SECTOR_PADDING \
    (SECTOR_PAYLOAD_BYTES - CONTACT_SECTOR_HEADER_BYTES - sizeof(SECTOR_TYPE) - \
     (CONTACT_SECTOR_CAPACITY * CONTACT_RECORD_BYTES))

#define CONTACT_MEMORY_SECTORS(numContacts, contactSecCapacity) \
    (((numContacts) + (contactSecCapacity) - 1) / (contactSecCapacity))

// Total sectors required to store HASH_TABLE_SIZE contacts.
#define CONTACT_MEMORY_SECTOR_SIZE \
    CONTACT_MEMORY_SECTORS(HASH_TABLE_SIZE, CONTACT_SECTOR_CAPACITY)

/* ---- Message region sizing --------------------------------------
 * Only what's needed for TOTAL_DATA_SECTOR_SIZE math. The MessageBlock
 * struct layout itself stays in message_extent.h.
 */
#define MESSAGE_HISTORY_SECTOR_CAPACITY \
    (SECTOR_PAYLOAD_BYTES / sizeof(uint16_t))

/* ---- Message region sizing --------------------------------------
 * Only what's needed for TOTAL_DATA_SECTOR_SIZE math. The MessageBlock
 * struct layout itself stays in message_extent.h.
 */
#define TOTAL_MESSAGE_SECTOR_SIZE (HASH_TABLE_SIZE * 2)


/* ---- Message History region sizing --------------------------------------
 * Only what's needed for TOTAL_DATA_SECTOR_SIZE math. The MessageBlock
 * struct layout itself stays in message_extent.h.
 */
#define TOTAL_MESSAGE_HISTORY_SECTOR_SIZE 0 // TO BE SET
/* ---- Call History region sizing --------------------------------------
 * Only what's needed for TOTAL_DATA_SECTOR_SIZE math. The MessageBlock
 * struct layout itself stays in message_extent.h.
 */
#define TOTAL_CALL_HISTORY_SECTOR_SIZE 0 // TO BE SET

/* ---- Overall data region ----------------------------------------*/
#define TOTAL_CONTACT_SECTOR_SIZE CONTACT_MEMORY_SECTOR_SIZE
#define TOTAL_DATA_SECTOR_SIZE (TOTAL_CONTACT_SECTOR_SIZE + TOTAL_MESSAGE_SECTOR_SIZE)

/* ================================================================
 * On-disk layout — where each region starts
 * ================================================================
 *
 *   SD Card
 *   +-------------+-----------+--------------+------------------------+-----------------+-----------------+
 *   | Superheader |  Journal  | Usage Bitmap | Contact + Message data | Message History |    Call History |
 *   | (1 sector)  | (3 sect.) | (N sectors)  |                        |    Ring Buffer  |    Ring Buffer  |
 *   +-------------+-----------+--------------+------------------------+-----------------+-----------------+
 *   0            1           1+3            N+4
 *
 * Every region's start is derived from the one before it, so there is
 * exactly one place that can get the offsets wrong.
 */

/* ---- Superheader — sector 0 ----------------------------------------*/
#define SUPERHEADER_SECTOR 0
#define SUPERHEADER_DATA_BYTES 16
#define SUPERHEADER_PADDING (SUPERHEADER_BYTES - SUPERHEADER_DATA_BYTES - sizeof(uint32_t))
#define SUPERHEADER_BYTES SECTOR_SIZE
#define SUPERHEADER_SECTOR_SIZE 1


/* ---- Journal — starts right after the superheader ----------------------------------------*/
#define JRNL_HEADER_SECTOR  (SUPERHEADER_SECTOR + SUPERHEADER_SECTOR_SIZE)
#define JRNL_HEADER_DATA_SIZE 8
#define JRNL_HEADER_PADDING (SECTOR_PAYLOAD_BYTES - JRNL_HEADER_DATA_SIZE)
#define JRNL_CONTENT_SECTOR (JRNL_HEADER_SECTOR + 1)
#define JRNL_USAGE_SECTOR   (JRNL_HEADER_SECTOR + 2)
#define JRNL_SECTOR_SIZE    3

/* ---- Usage bitmap — starts right after the journal ----------------------------------------*/
#define USAGE_BITMAP_START_SECTOR (JRNL_HEADER_SECTOR + JRNL_SECTOR_SIZE)

#define BITS_PER_ELEMENT 32
#define BYTES_PER_ELEMENT sizeof(uint32_t)
#define ELEMENTS_PER_SECTOR (SECTOR_SIZE / BYTES_PER_ELEMENT)

// The last word of every bitmap sector is its CRC trailer, so only the
// words before it hold usage bits.
#define USAGE_WORDS_PER_SECTOR (ELEMENTS_PER_SECTOR - SECTOR_CRC_BYTES / BYTES_PER_ELEMENT)
#define USAGE_BITS_PER_SECTOR  (USAGE_WORDS_PER_SECTOR * BITS_PER_ELEMENT)

// Number of 512B sectors needed to persist the usage bitmap.
#define USAGE_BITMAP_SECTOR_SIZE \
    ((TOTAL_DATA_SECTOR_SIZE + USAGE_BITS_PER_SECTOR - 1) / USAGE_BITS_PER_SECTOR)

// Number of uint32_t elements holding usage bits (excludes CRC trailers).
#define USAGE_BITMAP_SIZE \
    (USAGE_BITMAP_SECTOR_SIZE * USAGE_WORDS_PER_SECTOR)

// Total words allocated for the in-RAM usage bitmap array (sector-rounded, includes CRC trailers).
#define USAGE_BITMAP_STORAGE_SIZE \
    (USAGE_BITMAP_SECTOR_SIZE * ELEMENTS_PER_SECTOR)

// Total number of addressable usage bits
#define USAGE_BITMAP_TOTAL_BITS \
    (USAGE_BITMAP_SECTOR_SIZE * USAGE_BITS_PER_SECTOR)

// Usage bitmap addressing helpers.
// (dataSecInd is the Data Sector Index (Not SD sector index), therefore DATA_REGION_START_SECTOR == dataSecInd 0)
// FIND_ELEMENT is the word within its bitmap sector, FIND_INDEX is the word in the flat RAM array.
#define USAGE_BITMAP_FIND_SECTOR(dataSecInd)  ((dataSecInd) / USAGE_BITS_PER_SECTOR)
#define USAGE_BITMAP_FIND_ELEMENT(dataSecInd) (((dataSecInd) % USAGE_BITS_PER_SECTOR) / BITS_PER_ELEMENT)
#define USAGE_BITMAP_FIND_INDEX(dataSecInd) \
    (USAGE_BITMAP_FIND_SECTOR(dataSecInd) * ELEMENTS_PER_SECTOR + USAGE_BITMAP_FIND_ELEMENT(dataSecInd))
#define USAGE_BITMAP_FIND_BIT(dataSecInd)     ((dataSecInd) % BITS_PER_ELEMENT)

// Inverse of FIND_INDEX: data sector index of bit 0 of a flat RAM array word
#define USAGE_BITMAP_WORD_TO_INDEX(word) \
    (((word) / ELEMENTS_PER_SECTOR) * USAGE_BITS_PER_SECTOR + ((word) % ELEMENTS_PER_SECTOR) * BITS_PER_ELEMENT)

// True if a flat RAM array word is a sector CRC trailer rather than usage bits
#define USAGE_BITMAP_IS_CRC_WORD(word) (((word) % ELEMENTS_PER_SECTOR) >= USAGE_WORDS_PER_SECTOR)


/* ---- Data Regions — data type starting sectors ----------------------------------------*/
#define DATA_REGION_START_SECTOR   (USAGE_BITMAP_START_SECTOR + USAGE_BITMAP_SECTOR_SIZE)

// Start sector FROM DATA_REGION START SECTOR
#define CONTACT_DATA_START_SECTOR  0
#define MESSAGE_DATA_START_SECTOR  (CONTACT_DATA_START_SECTOR + TOTAL_CONTACT_SECTOR_SIZE)
#define MESSAGE_HISTORY_DATA_START_SECTOR (MESSAGE_DATA_START_SECTOR + TOTAL_MESSAGE_SECTOR_SIZE)
#define CALL_HISTORY_DATA_START_SECTOR (MESSAGE_HISTORY_DATA_START_SECTOR + TOTAL_MESSAGE_HISTORY_SECTOR_SIZE)

/* ---- Index conversions ----------------------------------------
 * Three index spaces exist:
 *   - record index:  contact slot (hash entry sector) or message sector index (latest_msg_extent)
 *   - data sector:   index from DATA_REGION_START_SECTOR, used by the usage bitmap and the journal
 *   - raw sector:    SD card block address
 */
#define CONTACT_DATA_SECTOR(slot)   (CONTACT_DATA_START_SECTOR + (slot) / CONTACT_SECTOR_CAPACITY)
#define MESSAGE_DATA_SECTOR(msgInd) (MESSAGE_DATA_START_SECTOR + (msgInd))
#define MESSAGE_HIST_DATA_SECTOR(mhInd) (MESSAGE_HISTORY_DATA_START_SECTOR + (mhInd) / MESSAGE_HISTORY_SECTOR_CAPACITY)
#define DATA_SECTOR_TO_RAW(dataSec) (DATA_REGION_START_SECTOR + (dataSec))

#ifdef __cplusplus
}
#endif

#endif
