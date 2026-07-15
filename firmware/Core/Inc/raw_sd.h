#include <stdint.h>


// initialise the sd card
int sd_raw_init(void);

// Deinit the sd card
int sd_raw_deinit(void);


int sd_raw_read(uint32_t sector, void *buffer, uint32_t sector_count);

int sd_raw_write(uint32_t sector, const void *buffer, uint32_t sector_count);

int sd_raw_sync(void);

uint32_t sd_raw_sector_size(void);
uint32_t sd_raw_sector_count(void);