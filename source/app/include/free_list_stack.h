#ifndef FREE_LIST_STACK_H
#define FREE_LIST_STACK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct  
{
    uint16_t *free_stack;
    size_t stack_top;
    size_t capacity;
    size_t used_count;
} FreeList;

/**
 * @brief Initialises the sector allocator.
 *
 * @param total_sectors Total number of sectors available in the system.
 * @retval true if initialisation succeeded
 */
bool free_list_init(FreeList *self, uint16_t *free_stack, size_t total_sectors);

/**
 * @brief Allocates a free sector.
 *
 * @retval sector index if successful
 * @retval UINT16_MAX if no sectors are available
 */
uint16_t free_list_allocate(FreeList *self);

/**
 * @brief Frees a previously allocated sector.
 *
 * @param sector Sector index to free
 */
void free_list_free(FreeList *self, uint16_t sector);

/**
 * @brief Returns number of available sectors.
 */
size_t free_list_available(FreeList *self);

/**
 * @brief Returns number of used sectors.
 */
size_t free_list_used(FreeList *self);

/**
 * @brief Resets allocator (all sectors become free again).
 */
void free_list_reset(FreeList *self);

bool free_list_init_software(FreeList* self, size_t total_sectors);

#ifdef __cplusplus
}
#endif

#endif /* FREE_LIST_STACK_H */
