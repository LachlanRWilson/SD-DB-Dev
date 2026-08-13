#include "contact.h"
#include "string.h"



/**
  * @brief  Create a Contact
  * @param  table: Hash Table struct being initialised
  * @param storage: 
  * @param  fstacks: pointer to array of FLSs (allowing multiple FLSs) 
  * @param  entries: In RAM storage of hash table entries
  * @param  size: number of elements in hash table
  */
ContactBuffer create_contact(const std::string& name, const std::string& phone)
{
    ContactBuffer contact;
    // Check phone and name len
    if (strlen(name) > MAX_NAME_LEN || strlen (phone) > MAX_PHONE_LEN) {
        return NULL;
    }

    contact.contact.name_len = strlen(name);
    memcpy(contact.contact.name, name, contact.contact.name_len);
    contact.contact.phone_len = strlen(phone);
    memcpy(contact.contact.phone, phone, contact.contact.phone_len);

    return contact;

}
