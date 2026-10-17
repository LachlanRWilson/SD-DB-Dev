#ifndef CARD_IMAGE_H
#define CARD_IMAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "database.h"

// Raw sectors spanned by the database (matches DATABASE_RAW_SECTOR_SIZE in database.c)
#define CARD_RAW_SECTORS ((size_t)(DATA_REGION_START_SECTOR + TOTAL_DATABASE_SECTOR_SIZE))
#define CARD_BYTES       (CARD_RAW_SECTORS * SECTOR_SIZE)

// Storage access counters (reset per command by the CLI)
typedef struct
{
    uint32_t reads;     // sectors read
    uint32_t writes;    // sectors written
    uint32_t failed;    // writes dropped by a simulated power cut
} CardStats;

extern CardStats card_stats;

/**
 * @brief Open (or create) the card image file and load it into database.c's
 * heap storage. A new or short image reads as a blank card.
 *
 * @param path image file path
 * @retval true if the image is open and loaded
 */
bool card_image_open(const char *path);

/**
 * @brief Write all of heap storage to the image file. Used after
 * database_init/database_format, which write before the wrapper is installed
 *
 * @retval true if the whole image was written
 */
bool card_image_flush(void);

/**
 * @brief Close the image file
 */
void card_image_close(void);

/**
 * @brief Replace the database's storage callbacks with ones that count every
 * access, write each sector through to the image file and can simulate a
 * power cut. Must be called after every database_init/database_format.
 *
 * @param db database struct pointer
 */
void card_image_wrap(Database *db);

/**
 * @brief Make the next `writes` sector writes succeed and every write after
 * them fail, as if power was cut. A negative value disables the cut.
 *
 * @param writes writes allowed before the cut
 */
void card_image_cut_after(int writes);

#endif /* CARD_IMAGE_H */
