#include <string.h>
#include "contact.h"
#include "journal.h"
#include "usage_bitmap.h"

/**
 * @brief Read the contact sector that the contact in stored in on the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param index memory index of the contact.
 * @param out Contact Sector Buffer with the desired contact position
 * @retval True if successful read else false.
 */
bool read_contact_sector(Storage *storage, uint16_t index, ContactSectorBuffer *out)
{
    return read_sector(storage, (index / CONTACT_SECTOR_CAPACITY) + CONTACT_DATA_START_SECTOR, out->buffer);
}

/**
 * @brief Write the contact sector that the contact in stored in on the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param index memory index of the contact.
 * @param in Contact Sector Buffer going into the SD card
 * @retval True if successful write else false.
 */
bool write_contact_sector(Storage *storage, uint16_t index, ContactSectorBuffer *in)
{
    return write_sector(storage, (index / CONTACT_SECTOR_CAPACITY) + CONTACT_DATA_START_SECTOR, in->buffer);
}


/**
 * @brief Write the contact to the sd card in the appropriate contact sector
 *
 * @param storage Pointer to the storage abstraction.
 * @param journal pointer to rollback journal struct
 * @param index memory index of the contact.
 * @param in Contact Sector Buffer going into the SD card
 * @retval True if successful write else false.
 */
bool write_contact(Storage *storage, Journal *journal, uint16_t index, ContactBuffer *in)
{
    ContactSectorBuffer cSector;
    uint8_t contactPosInSector;

    /*
     * FIXME(usage-bitmap): the usage bitmap tracks one bit per *physical
     * contact sector*, not per contact slot. The write below sets the bit
     * at (index / CONTACT_SECTOR_CAPACITY), so every check/clear site must
     * use the same mapping. Previously these sites passed the raw `index`,
     * so read_contact()/remove_contact() tested a bit write_contact() never
     * set and always reported "not found" straight after an insert.
     */
    // Check if contact sector is being used
    if (check_usage_bit(index / CONTACT_SECTOR_CAPACITY)) {

        // Read contact sector
        if (!read_contact_sector(storage, index, &cSector))
        {
            return false;
        }
    } else {
        /*
         * Sector not in use yet: start from a clean slate. Without this the
         * other CONTACT_SECTOR_CAPACITY - 1 slots (and the used_bitmap
         * header) keep whatever stack garbage cSector was declared with,
         * which then gets persisted and confuses hash_reconstruct_contact.
         */
        memset(&cSector, 0, sizeof(cSector));
    }

    // Add to journal for rollback and update usage bit on SD card
    if (!journal_add(journal, JRNL_CONTACT, index, cSector.buffer))
    {
        return false;
    }

    // update usage bit in ram and sd
    if(!update_usage_bit(storage, (index / CONTACT_SECTOR_CAPACITY), true))
    {
        return false;
    }

    // Get the position in the contact sector
    contactPosInSector = index % CONTACT_SECTOR_CAPACITY;

    // Set contact bit
    cSector.sector.header.used_bitmap |= (1 << contactPosInSector);

    // Write contact to sector
    memcpy(cSector.sector.contacts[contactPosInSector].buffer, in->buffer, sizeof(Contact));

    // Write contact sector to SD card, if sector write doesn't fail free journal. If either fail
    // return false
    if (!write_contact_sector(storage, index, &cSector) || !journal_free(journal))
    {
        return false;
    }

    return true;
}

/**
 * @brief Read the contact to the sd card from the appropriate contact sector
 *
 * @param storage Pointer to the storage struct
 * @param index memory index of the contact.
 * @param in Contact Sector Buffer going into the SD card
 * @retval True if successful write else false.
 */
bool read_contact(Storage *storage, uint16_t index, ContactBuffer *out)
{
    ContactSectorBuffer cSector;
    uint8_t contactPosInSector;

    // If usage bit isn't being used then trying to read empty sector
    // (see FIXME(usage-bitmap) in write_contact: bit is per contact sector)
    if (!check_usage_bit(index / CONTACT_SECTOR_CAPACITY))
    {
        return false;
    }

    // read contact sector
    if(!read_contact_sector(storage, index, &cSector))
    {
        return false;
    }

    // Get the position in the contact sector
    contactPosInSector = index % CONTACT_SECTOR_CAPACITY;

    // Check contact use bit
   if (!(cSector.sector.header.used_bitmap & (1 << contactPosInSector)))
   {
       return false;
   }

   // Write contact buffer in sector to output buffer
   memcpy(out->buffer, cSector.sector.contacts[contactPosInSector].buffer, sizeof(Contact));
   return true;
}

/**
 * @brief Remove the contact to the sd card from the appropriate contact sector
 *
 * @param storage Pointer to the storage struct
 * @param index memory index of the contact.
 * @param in Contact Sector Buffer going into the SD card
 * @retval True if successful write else false.
 */
bool remove_contact(Storage *storage, Journal *journal, uint16_t index, ContactBuffer *out)
{
    ContactSectorBuffer cSector;
    uint8_t contactPosInSector;

    // check the usage bit for the contact sector is set if there is no contact sector than there is not contact
    if (!check_usage_bit(index / CONTACT_SECTOR_CAPACITY))
    {
        return false;
    }

    // read contact sector
    if(!read_contact_sector(storage, index, &cSector))
    {
        return false;
    }

    // Add contact to jounral
    if (!journal_add(journal, JRNL_CONTACT, index, cSector.buffer))
    {
        return false;
    }

    // Get the position in the contact sector
    contactPosInSector = index % CONTACT_SECTOR_CAPACITY;

    // Check contact use bit
   if (!(cSector.sector.header.used_bitmap & (1 << contactPosInSector)))
   {
       return false;
   }

   // Write contact buffer in sector to output buffer
   memcpy(out->buffer, cSector.sector.contacts[contactPosInSector].buffer, sizeof(Contact));

   // Unset used bit
    cSector.sector.header.used_bitmap &= ~(1 << contactPosInSector);

    // If there is no contact in the sector set as empty
    if (cSector.sector.header.used_bitmap == 0)
    {
        // set the sector empty since there are no contact left in the sector
        if (!update_usage_bit(storage, index / CONTACT_SECTOR_CAPACITY, false)) {return false;}

    } else {

        // Write updated sector back to SD card
        if (!write_contact_sector(storage, index, &cSector)) {return false;}
    }

   return true;

}

/**
  * @brief  Create a Contact
  * @param storage Pointer to the storage struct
  * @param  fstacks: pointer to array of FLSs (allowing multiple FLSs)
  * @param  entries: In RAM storage of hash table entries
  * @param  size: number of elements in hash table
  */
ContactBuffer create_contact(const char *name, const char *phone)
{
    ContactBuffer contact = {0};
    // Check phone and name len
    if (strlen(name) > MAX_NAME_LEN || strlen (phone) > MAX_PHONE_LEN) {
        return contact;
    }

    contact.contact.name_len = strlen(name);
    memcpy(contact.contact.name, name, contact.contact.name_len);
    contact.contact.phone_len = strlen(phone);
    memcpy(contact.contact.phone, phone, contact.contact.phone_len);

    return contact;

}
