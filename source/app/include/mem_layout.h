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

/* ---- Contact sector layout ------------------------------------- */

// How many Contact records fit in one 512B sector, after the sector
// type tag and the ContactSectorHeader.
#define CONTACT_SECTOR_CAPACITY \
    ((SECTOR_SIZE - CONTACT_SECTOR_HEADER_BYTES - sizeof(SECTOR_TYPE)) / CONTACT_RECORD_BYTES)

#define CONTACT_SECTOR_PADDING \
    (SECTOR_SIZE - CONTACT_SECTOR_HEADER_BYTES - sizeof(SECTOR_TYPE) - \
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
#define TOTAL_MESSAGE_SECTOR_SIZE (HASH_TABLE_SIZE * 2)

/* ---- Overall data region ----------------------------------------*/
#define TOTAL_CONTACT_SECTOR_SIZE CONTACT_MEMORY_SECTOR_SIZE
#define TOTAL_DATA_SECTOR_SIZE (TOTAL_CONTACT_SECTOR_SIZE + TOTAL_MESSAGE_SECTOR_SIZE)

/* ================================================================
 * On-disk layout — where each region starts
 * ================================================================
 *
 *   SD Card
 *   +------------+---------------+---------+------------------------+
 *   | Superheader| Usage Bitmap  | Journal | Contact + Message data |
 *   | (1 sector) | (N sectors)   |(3 sect.)|                        |
 *   +------------+---------------+---------+------------------------+
 *   0            1               1+N       1+N+3
 *
 * Every region's start is derived from the one before it, so there is
 * exactly one place that can get the offsets wrong.
 */

// Superheader — sector 0
#define SUPERHEADER_SECTOR 0
#define SUPERHEADER_DATA_BYTES 12
#define SUPERHEADER_PADDING (SUPERHEADER_BYTES - SUPERHEADER_DATA_BYTES - 2 * sizeof(uint32_t))
#define SUPERHEADER_BYTES SECTOR_SIZE
#define SUPERHEADER_SECTOR_SIZE 1

// Usage bitmap — starts right after the superheader
#define USAGE_BITMAP_START_SECTOR (SUPERHEADER_SECTOR + SUPERHEADER_SECTOR_SIZE)

#define BITS_PER_ELEMENT 32
#define BYTES_PER_ELEMENT sizeof(uint32_t)
#define ELEMENTS_PER_SECTOR (SECTOR_SIZE / BYTES_PER_ELEMENT)

// Number of uint32_t elements needed to hold one bit per data sector.
#define USAGE_BITMAP_SIZE \
    ((TOTAL_DATA_SECTOR_SIZE + BITS_PER_ELEMENT - 1) / BITS_PER_ELEMENT)

// Number of 512B sectors needed to persist the usage bitmap.
#define USAGE_BITMAP_SECTOR_SIZE \
    ((USAGE_BITMAP_SIZE + ELEMENTS_PER_SECTOR - 1) / ELEMENTS_PER_SECTOR)

// Total words allocated for the in-RAM usage bitmap array (sector-rounded).
#define USAGE_BITMAP_STORAGE_SIZE \
    (USAGE_BITMAP_SECTOR_SIZE * ELEMENTS_PER_SECTOR)

// Usage bitmap addressing helpers.
#define USAGE_BITMAP_FIND_SECTOR(sectorInd)  ((sectorInd) / (SECTOR_SIZE * 8U))
#define USAGE_BITMAP_FIND_ELEMENT(sectorInd) (((sectorInd) % (SECTOR_SIZE * 8U)) / BITS_PER_ELEMENT)
#define USAGE_BITMAP_FIND_INDEX(sectorInd)   ((sectorInd) / BITS_PER_ELEMENT)
#define USAGE_BITMAP_FIND_BIT(sectorInd)     ((sectorInd) % BITS_PER_ELEMENT)

// Journal — starts right after the usage bitmap.
#define JRNL_HEADER_SECTOR  (USAGE_BITMAP_START_SECTOR + USAGE_BITMAP_SECTOR_SIZE)
#define JRNL_CONTENT_SECTOR (JRNL_HEADER_SECTOR + 1)
#define JRNL_USAGE_SECTOR   (JRNL_HEADER_SECTOR + 2)
#define JRNL_SECTOR_SIZE    3

// Data region (contacts, then messages) — starts right after the journal.
// This is the offset contact.c / message_extent.c were missing, which
// caused physical sector collisions with the superheader/bitmap/journal.
#define DATA_REGION_START_SECTOR   (JRNL_HEADER_SECTOR + JRNL_SECTOR_SIZE)
#define CONTACT_DATA_START_SECTOR  DATA_REGION_START_SECTOR
#define MESSAGE_DATA_START_SECTOR  (CONTACT_DATA_START_SECTOR + TOTAL_CONTACT_SECTOR_SIZE)

#ifdef __cplusplus
}
#endif

#endif
