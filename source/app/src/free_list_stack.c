#include "free_list_stack.h"

#include <stdlib.h>


bool free_list_init(FreeList *self, uint16_t *free_stack, size_t total_sectors)
{
    if (total_sectors == 0)
    {
        return false;
    }

    // Set Free List Stack attributes
    self->free_stack = free_stack;
    self->capacity = total_sectors;
    self->stack_top = total_sectors;
    self->used_count = 0;


    // Initialise sector indexes
    for (uint16_t i = 0; i < total_sectors; i++)
    {
        self->free_stack[i] = i;
    }

    return true;
}

/**
 * @brief initialise an empty free list
 *
 * @param self FreeList struct pointer
 * @param free_stack pointer to free list stack memory
 * @param total_sectors number of sectors the free list stack needs to track
 * 
 * @return false if fail, else true
 */
bool free_list_empty_init(FreeList *self, uint16_t *free_stack, size_t total_sectors)
{
    if (self == NULL || free_stack == NULL || total_sectors == 0)
    {
        return false;
    }

    // Set Free List Stack attributes
    self->free_stack = free_stack;
    self->capacity = total_sectors;
    self->stack_top = 0; // start stack at the bottom
    self->used_count = total_sectors; // all sectors used

    return true;
}

uint16_t free_list_allocate(FreeList *self)
{
    // Check stack is allocated and not at bottom (should never reach bottom)
    if (self == NULL || self->free_stack == NULL || self->stack_top == 0)
    {
        return UINT16_MAX;
    }

    uint16_t sector = self->free_stack[--self->stack_top];
    self->used_count++;

    return sector;
}

void free_list_free(FreeList *self, uint16_t sector)
{
    if (self->free_stack == NULL || self->stack_top >= self->capacity)
    {
        return;
    }

    self->free_stack[self->stack_top++] = sector;

    if (self->used_count > 0)
    {
        self->used_count--;
    }
}

/**
 * @brief Frees range of sectors [startSector, endSector)
 *
 * @param self free list stack instance
 * @param startSector inclusive start sector
 * @param endSector exclusive end sector
 */
void free_list_free_range(FreeList *self, uint16_t startSector, uint16_t endSector)
{
    for (int i = startSector; i < endSector; i++)
    {
        // free index has reached passed the number of sectors allocated
        if (i >= self->capacity)
        {
            return;
        }

        free_list_free(self, i);
    }
}

size_t free_list_available(FreeList *self)
{
    return self->stack_top;
}

size_t free_list_used(FreeList *self)
{
    return self->used_count;
}

void free_list_reset(FreeList *self)
{
    if (self->free_stack == NULL)
    {
        return;
    }

    self->stack_top = self->capacity;
    self->used_count = 0;

    for (uint16_t i = 0; i < self->capacity; i++)
    {
        self->free_stack[i] = i;
    }
}

bool free_list_init_software(FreeList* self, size_t total_sectors)
{
    if (total_sectors == 0)
    {
        return false;
    }

    self->free_stack = (uint16_t *)malloc(total_sectors * sizeof(uint16_t));
    if (self->free_stack == NULL)
    {
        return false;
    }

    self->capacity = total_sectors;
    self->stack_top = total_sectors;
    self->used_count = 0;

    for (uint16_t i = 0; i < total_sectors; i++)
    {
        self->free_stack[i] = i;
    }

    return true;
}
