#ifndef CONTACT_H
#define CONTACT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "mem_layout.h"
#include "storage.h"

#if defined(__cplusplus)
    #define STATIC_ASSERT static_assert
#else
    #define STATIC_ASSERT _Static_assert
#endif


// Contact Information Max Length
#define MAX_NAME_LEN 64
#define MAX_PHONE_LEN 15


// Opaque Declaration
typedef struct Journal Journal;

// Contact Struct (81B)
typedef struct
{
    uint8_t name_len;          /**< Length of name string (1B)*/
    char name[MAX_NAME_LEN];   /**< Contact name (64B)*/
    uint8_t phone_len;         /**< Length of phone number (1B) */
    char phone[MAX_PHONE_LEN]; /**< Phone number (15B)*/
} Contact;

typedef union {
   Contact contact;
   uint8_t buffer[sizeof(Contact)];
} ContactBuffer;

// Contact Block Header (2B)
typedef struct
{
    uint8_t used_bitmap; // 1B

} ContactSectorHeader;

// Contact Sector needs to be 512 bytes since smallest read and write size is 512B
typedef struct
{
    SECTOR_TYPE type; // 1B
    ContactSectorHeader header; // Header MUST be first
    ContactBuffer contacts[CONTACT_SECTOR_CAPACITY];
    uint8_t padding[CONTACT_SECTOR_PADDING];
} ContactSector;

typedef union {
    ContactSector sector;
    uint8_t buffer[sizeof(ContactSector)];
} ContactSectorBuffer;

STATIC_ASSERT(sizeof(Contact) == CONTACT_RECORD_BYTES, "Unexpected Contact size");
STATIC_ASSERT(sizeof(ContactSector) == SECTOR_SIZE, "ContactSector struct is not 512B");

/**
 * @brief Read the contact sector that the contact in stored in on the sd card
 *
 * @param table Pointer to the hash table.
 * @param index memory index of the contact.
 * @param out Contact Sector Buffer with the desired contact position
 * @retval True if successful read else false.
 */
bool read_contact_sector(Storage *storage, uint16_t index, ContactSectorBuffer *out);

/**
 * @brief Write the contact sector that the contact in stored in on the sd card
 *
 * @param table Pointer to the hash table.
 * @param index memory index of the contact.
 * @param in Contact Sector Buffer going into the SD card
 * @retval True if successful write else false.
 */
bool write_contact_sector(Storage *storage, uint16_t index, ContactSectorBuffer *in);

/**
 * @brief Write the contact to the sd card in the appropriate contact sector
 *
 * @param table Pointer to the hash table.
 * @param journal pointer to rollback journal struct
 * @param index memory index of the contact.
 * @param in Contact Sector Buffer going into the SD card
 * @retval True if successful write else false.
 */
bool write_contact(Storage *storage, Journal *journal, uint16_t index, ContactBuffer *in);

/**
 * @brief Read the contact to the sd card from the appropriate contact sector
 *
 * @param table Pointer to the hash table.
 * @param index memory index of the contact.
 * @param in Contact Sector Buffer going into the SD card
 * @retval True if successful write else false.
 */
bool read_contact(Storage *storage, uint16_t index, ContactBuffer *out);

/**
 * @brief Remove the contact to the sd card from the appropriate contact sector
 *
 * @param table Pointer to the hash table.
 * @param index memory index of the contact.
 * @param in Contact Sector Buffer going into the SD card
 * @retval True if successful write else false.
 */
bool remove_contact(Storage *storage, Journal *journal, uint16_t index, ContactBuffer *out);



#ifdef __cplusplus
}
#endif

#endif /* CONTACT_H */
