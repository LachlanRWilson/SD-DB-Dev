#ifndef CONTACT_H
#define CONTACT_H

#include "stdint.h"
#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__cplusplus)
    #define STATIC_ASSERT static_assert
#else
    #define STATIC_ASSERT _Static_assert
#endif

#define CONTACT_SECTOR_BYTES 512
#define CONTACT_HEADER_BYTES 1
#define CONTACT_BYTES 81

#define MAX_NAME_LEN 64
#define MAX_PHONE_LEN 15

// Get the number of contacts that can be stored in the ContactSector
#define CONTACT_SECTOR_CAPACITY \
    ((CONTACT_SECTOR_BYTES - sizeof(ContactSectorHeader)) / sizeof(Contact))

// calculate the padding of the sector
#define CONTACT_SECTOR_PADDING \
    (CONTACT_SECTOR_BYTES - sizeof(ContactSectorHeader) - CONTACT_SECTOR_CAPACITY * sizeof(Contact))

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

// Contact Block Header (8B)
typedef struct 
{
    uint8_t used_bitmap; // 1B
} ContactSectorHeader;

// Contact Sector needs to be 512 bytes since smallest read and write size is 512B
typedef struct 
{
    ContactBuffer contacts[CONTACT_SECTOR_CAPACITY];
    ContactSectorHeader header;
    uint8_t padding[CONTACT_SECTOR_PADDING];
} ContactSector;

typedef union {
    ContactSector sector;
    uint8_t buffer[sizeof(ContactSector)];
} ContactSectorBuffer;

STATIC_ASSERT(sizeof(Contact) == CONTACT_BYTES, "Unexpected Contact size");

#ifdef __cplusplus
}
#endif

#endif /* CONTACT_H */
