#ifndef NI_SD_BASELINE_H
#define NI_SD_BASELINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/**
 * SD card baseline (target only): the cost of the SD access that accompanies
 * each real index update, measured with the project's Storage and journal
 * code. Writes go to the DB region of the card (its contents are disposable).
 */

typedef enum {
    SDB_READ_BLOCK = 0,       // storage->read_block(), one 512B block
    SDB_WRITE_BLOCK,          // storage->write_block(), one 512B block
    SDB_READ_SECTOR,          // read_sector(): block read + CRC check
    SDB_WRITE_CONTACT,        // write_contact(): journal pre-image + write + bitmap
    SDB_COUNT
} SdbOp;

bool sd_baseline_init(void);

const char *sd_baseline_name(SdbOp op);

uint32_t sd_baseline_measure(SdbOp op, uint32_t *cycles, uint32_t reps);

#ifdef __cplusplus
}
#endif

#endif
