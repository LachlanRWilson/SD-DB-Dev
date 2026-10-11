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
 * Database Structs Size Defines
 * ================================================================ */

/* Hash Table */
#define HASH_TABLE_SIZE 14293
#define HASH_TABLE_ENTRIES 10000
// Byte size of the HashEntry struct (see hash_table.h). Only needed
// here for layout math — the struct itself still lives in hash_table.h.
#define HASH_ENTRY_BYTES 8


/* Message History */

/* Ring Buffer */

/* ------------------ CONTACT INFORMATION ----------------------- */

#define CONTACT_RECORD_BYTES 81
#define CONTACT_SECTOR_HEADER_BYTES 1

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

#define TOTAL_CONTACT_SECTOR_SIZE CONTACT_MEMORY_SECTOR_SIZE

/* ------------------ MESSAGE INFORMATION ----------------------- */
#define MESSAGE_SECTOR_SIZE 2 * HASH_TABLE_SIZE

#define SMS_MAX_MESSAGE_LENGTH 160
#define MAX_PHONE_LEN 15
#define MESSAGE_BYTES 164
#define MESSAGE_SECTOR_HEADER_BYTES 24
#define MESSAGE_SECTOR_BYTES SECTOR_SIZE

// Fit messages into a 512B block of memory
#define MESSAGE_SECTOR_CAPACITY \
    ((SECTOR_PAYLOAD_BYTES - MESSAGE_SECTOR_HEADER_BYTES) / MESSAGE_BYTES)

// Ensure padding is accounted for
#define MESSAGE_SECTOR_PADDING \
    (SECTOR_PAYLOAD_BYTES - MESSAGE_SECTOR_HEADER_BYTES - (MESSAGE_SECTOR_CAPACITY * MESSAGE_BYTES))

// Message Memory Region Sector Size
#define TOTAL_MESSAGE_SECTOR_SIZE (HASH_TABLE_SIZE * 2)

/* ------------------ RING BUFFER INFORMATION ----------------------- */
#define RING_BUFFER_HEADER_BYTES 8
#define RING_BUFFER_PAYLOAD_BYTES \
    (SECTOR_PAYLOAD_BYTES - RING_BUFFER_HEADER_BYTES)
#define RING_BUFFER_SECTOR_BYTES SECTOR_SIZE

/* ------------------ MESSAGE HISTORY INFORMATION ----------------------- */
#define MESSAGE_INDEX_BYTES 2
#define MESSAGE_HISTORY_ENTRY_BYTES (MESSAGE_BYTES + MESSAGE_INDEX_BYTES)
#define MESSAGE_HISTORY_SECTOR_BYTES SECTOR_SIZE

#define MESSAGE_HISTORY_SECTOR_CAPACITY \
    (RING_BUFFER_PAYLOAD_BYTES / MESSAGE_HISTORY_ENTRY_BYTES)

#define MESSAGE_HISTORY_SECTOR_PADDING \
    (RING_BUFFER_PAYLOAD_BYTES - (MESSAGE_HISTORY_SECTOR_CAPACITY * MESSAGE_HISTORY_ENTRY_BYTES))

// Number of sectors for 10000 latest message history
#define TOTAL_MESSAGE_HISTORY_SECTOR_SIZE \
((HASH_TABLE_ENTRIES + MESSAGE_HISTORY_SECTOR_CAPACITY - 1) / MESSAGE_HISTORY_SECTOR_CAPACITY)

/* ------------------ CALL HISTORY INFORMATION ----------------------- */

#define CONTACT_INDEX_BYTES 2 // uint16_t

#define CALL_HISTORY_ENTRY_BYTES 84

#define CALL_HISTORY_SECTOR_CAPACITY \
    (RING_BUFFER_PAYLOAD_BYTES / CALL_HISTORY_ENTRY_BYTES)

#define CALL_HISTORY_SECTOR_PADDING \
    (RING_BUFFER_PAYLOAD_BYTES - (CALL_HISTORY_SECTOR_CAPACITY * CALL_HISTORY_ENTRY_BYTES))

#define TOTAL_CALL_HISTORY_SECTOR_SIZE 5000

/* ---- Overall data region covered by usage bitmap ----------------------------------------*/
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
#define MESSAGE_HIST_DATA_SECTOR(mhInd) (MESSAGE_HISTORY_DATA_START_SECTOR + (mhInd))
#define CALL_HIST_DATA_SECTOR(chInd) (CALL_HISTORY_DATA_START_SECTOR + (chInd))
#define DATA_SECTOR_TO_RAW(dataSec) (DATA_REGION_START_SECTOR + (dataSec))

#ifdef __cplusplus
}
#endif

#endif
