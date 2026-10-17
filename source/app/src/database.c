#include <string.h>

#include "database.h"
#include "superheader.h"
#include "usage_bitmap.h"

// Raw sectors spanned by the database (superheader -> end of call history)
#define DATABASE_RAW_SECTOR_SIZE (DATA_REGION_START_SECTOR + TOTAL_DATABASE_SECTOR_SIZE)

#ifndef HOST_BUILD
#include "sd_storage.h"
SDStorageContext ctx;

__attribute__((section(".ram_d1"))) HashEntry htable_mem[HASH_TABLE_SIZE];
__attribute__((section(".ram_d1"))) uint16_t contact_fls_mem[HASH_TABLE_ENTRIES];
__attribute__((section(".ram_d1"))) uint16_t message_fls_mem[TOTAL_MESSAGE_SECTOR_SIZE];
__attribute__((section(".ram_d1"))) SortedArrayEntry sa_mem[SORTED_ARRAY_ENTRIES];
#else

#include "heap_storage.h"
uint8_t heap_mem[DATABASE_RAW_SECTOR_SIZE * SECTOR_SIZE];
HeapStorageContext ctx;
HashEntry htable_mem[HASH_TABLE_SIZE];
uint16_t contact_fls_mem[HASH_TABLE_ENTRIES];
uint16_t message_fls_mem[TOTAL_MESSAGE_SECTOR_SIZE];
SortedArrayEntry sa_mem[SORTED_ARRAY_ENTRIES];
#endif


/**
 * @brief Check a string is non NULL and no longer than max
 *
 * @param s input string
 * @param max max string length
 * @retval true if valid
 */
static bool valid_str(const char *s, size_t max)
{
    return (s != NULL) && (strnlen(s, max + 1) <= max);
}

/**
 * @brief Check a phone number is non empty and short enough to stay null
 * terminated in Contact.phone (strcmp is used on the stored phone)
 *
 * @param phone phone number
 * @retval true if valid
 */
static bool valid_phone(const char *phone)
{
    return valid_str(phone, MAX_PHONE_LEN - 1) && (phone[0] != '\0');
}

/**
 * @brief Get the hash slot index of a hash entry
 *
 * @param entry hash entry pointer (in htable_mem)
 * @retval hash slot index
 */
static inline uint16_t slot_of(const HashEntry *entry)
{
    return (uint16_t)(entry - htable_mem);
}

/**
 * @brief Find the occupied hash entry of a phone number
 *
 * @param db database struct pointer
 * @param phone phone number
 * @param entry found hash entry
 * @retval STRG_OK if found, else STRG_EMPTY
 */
static STRG_RET find_occupied(Database *db, const char *phone, HashEntry **entry)
{
    if (!hash_find_entry(&db->htable, phone, entry) || ((*entry)->state != ENTRY_OCCUPIED))
    {
        return STRG_EMPTY;
    }

    return STRG_OK;
}

/**
 * @brief Create a sorted array key from a contact name. Must match make_key()
 * in sorted_array.c
 *
 * @param contact contact the key is made from
 * @param out pointer to key array
 */
static void name_key(const Contact *contact, char out[KEY_LEN])
{
    for (size_t i = 0; i < KEY_LEN; i++)
    {
        char c = (i < contact->name_len) ? contact->name[i] : '\0';
        out[i] = ((c >= 'A') && (c <= 'Z')) ? (char)(c + ('a' - 'A')) : c;
    }
}

/**
 * @brief Rebuild the name index from the hash table. Every contact is appended
 * in hash slot order then sorted once
 *
 * @param db database struct pointer
 * @retval STRG_RET
 */
static STRG_RET name_index_build(Database *db)
{
    ContactBuffer contact;
    SortedArrayEntry entry;
    STRG_RET ret;

    if (!sorted_array_init(&db->name_index, htable_mem, &db->storage, sa_mem))
    {
        return STRG_FAIL;
    }

    for (uint16_t slot = 0; slot < HASH_TABLE_SIZE; slot++)
    {
        if (htable_mem[slot].state != ENTRY_OCCUPIED)
        {
            continue;
        }

        ret = read_contact(&db->storage, htable_mem[slot].sector, &contact);
        if (ret != STRG_OK)
        {
            return ret;
        }

        name_key(&contact.contact, entry.key);
        entry.hash_slot = slot;

        if (!sorted_array_append(&db->name_index, &entry))
        {
            return STRG_FULL;
        }
    }

    return sorted_array_sort(&db->name_index);
}

/**
 * @brief Find the head of the message and call history ring buffers
 *
 * @param db database struct pointer
 * @retval STRG_RET
 */
static STRG_RET history_init(Database *db)
{
    if (!init_ring_buffer(&db->storage, &db->message_history, TOTAL_MESSAGE_HISTORY_SECTOR_SIZE,
                DATA_SECTOR_TO_RAW(MESSAGE_HISTORY_DATA_START_SECTOR)))
    {
        return STRG_FAIL;
    }

    if (!init_ring_buffer(&db->storage, &db->call_history, TOTAL_CALL_HISTORY_SECTOR_SIZE,
                DATA_SECTOR_TO_RAW(CALL_HISTORY_DATA_START_SECTOR)))
    {
        return STRG_FAIL;
    }

    return STRG_OK;
}

/**
 * @brief Write every sector of a ring buffer region as RB_EMPTY
 *
 * @param storage pointer to storage abstraction struct
 * @param raw_sector_start raw sector of the first ring buffer sector
 * @param size number of sectors in the ring buffer
 * @retval STRG_RET
 */
static STRG_RET ring_buffer_clear(Storage *storage, uint16_t raw_sector_start, uint16_t size)
{
    U_RingBufferSector empty;
    STRG_RET ret;

    memset(&empty, 0, sizeof(empty));
    empty.sector.header.state = RB_EMPTY;

    for (uint16_t i = 0; i < size; i++)
    {
        ret = write_sector(storage, raw_sector_start + i, empty.buffer);
        if (ret != STRG_OK)
        {
            return ret;
        }
    }

    return STRG_OK;
}

/**
 * @brief Recover the journal and rebuild the in RAM structures from storage
 *
 * @param db database struct pointer
 * @retval STRG_RET
 */
static STRG_RET database_mount(Database *db)
{
    STRG_RET ret;

    // rollback must happen before any data (including the usage bitmap) is read
    memset(&db->journal, 0, sizeof(db->journal));
    if (!journal_init(&db->journal, &db->storage))
    {
        return STRG_FAIL;
    }

    // Allocator Initialisation (reconstruction frees unused indexes back onto the stacks)
    memset(htable_mem, 0, sizeof(htable_mem));
    if (!free_list_empty_init(&db->contact_allocator, contact_fls_mem, HASH_TABLE_ENTRIES) ||
        !free_list_empty_init(&db->message_allocator, message_fls_mem, TOTAL_MESSAGE_SECTOR_SIZE))
    {
        return STRG_FAIL;
    }

    // Hash Table Initialisation
    hash_init(&db->htable, &db->storage, &db->contact_allocator, &db->message_allocator, htable_mem, HASH_TABLE_SIZE);

    // Reconstruct hash table from SD card
    if (!read_usage_bitmap(&db->storage))
    {
        return STRG_FAIL;
    }

    if (!hash_reconstruct_contact(&db->htable) || !hash_reconstruct_message(&db->htable, &db->journal))
    {
        return STRG_FAIL;
    }

    ret = name_index_build(db);
    if (ret != STRG_OK)
    {
        return ret;
    }

    return history_init(db);
}

/**
 * @brief Initialise the storage and mount the database. If the superheader is
 * uninitialised the storage is formatted, otherwise the journal is recovered and
 * the in RAM structures are rebuilt from storage.
 *
 * @param db database struct pointer
 * @retval STRG_OK database mounted or formatted
 * @retval STRG_CORRUPT superheader corrupted or outdated (call database_format to wipe)
 * @retval STRG_FAIL storage or rebuild failure
 */
STRG_RET database_init(Database *db)
{
    SuperHeaderBuffer superheader;

    if (db == NULL)
    {
        return STRG_FAIL;
    }

    memset(db, 0, sizeof(*db));

    // set the storage abstraction context
#ifndef HOST_BUILD
    if (!SDStorage_Init(&ctx, 0, SECTOR_SIZE))
    {
        return STRG_FAIL;
    }
    sd_storage.context = (void *) &ctx;
    db->storage = sd_storage;
#else
    if (!HeapStorage_Init(&ctx, heap_mem, SECTOR_SIZE, DATABASE_RAW_SECTOR_SIZE))
    {
        return STRG_FAIL;
    }
    heap_storage.context = (void *) &ctx;
    db->storage = heap_storage;
#endif

    switch (superheader_check(&db->storage, &superheader))
    {
        case SUPR_GOOD:
            return database_mount(db);

        case SUPR_UNINITIALISED:
            return database_format(db);

        // don't wipe user data without being asked
        case SUPR_CORRUPTED:
        case SUPR_OUTDATED:
            return STRG_CORRUPT;

        case SUPR_FAIL:
        default:
            return STRG_FAIL;
    }
}

/**
 * @brief Format the storage to an empty database. Writes the journal, usage
 * bitmap, empty ring buffers and then the superheader.
 *
 * @param db database struct pointer (storage must be initialised)
 * @retval STRG_RET
 */
STRG_RET database_format(Database *db)
{
    SuperHeaderBuffer superheader;
    STRG_RET ret;

    if (db == NULL)
    {
        return STRG_FAIL;
    }

    // journal header written directly, a blank card reads as a corrupt journal
    memset(&db->journal, 0, sizeof(db->journal));
    db->journal.storage = &db->storage;
    if (!journal_header_init(&db->journal))
    {
        return STRG_FAIL;
    }

    if (!init_usage_bitmap(&db->storage))
    {
        return STRG_FAIL;
    }

    ret = ring_buffer_clear(&db->storage, DATA_SECTOR_TO_RAW(MESSAGE_HISTORY_DATA_START_SECTOR),
            TOTAL_MESSAGE_HISTORY_SECTOR_SIZE);
    if (ret != STRG_OK)
    {
        return ret;
    }

    ret = ring_buffer_clear(&db->storage, DATA_SECTOR_TO_RAW(CALL_HISTORY_DATA_START_SECTOR),
            TOTAL_CALL_HISTORY_SECTOR_SIZE);
    if (ret != STRG_OK)
    {
        return ret;
    }

    // superheader last so an interrupted format is redone on the next boot
    ret = superheader_init(&db->storage, &superheader);
    if (ret != STRG_OK)
    {
        return ret;
    }

    // Allocator Initialisation
    memset(htable_mem, 0, sizeof(htable_mem));
    if (!free_list_init(&db->contact_allocator, contact_fls_mem, HASH_TABLE_ENTRIES) ||
        !free_list_init(&db->message_allocator, message_fls_mem, TOTAL_MESSAGE_SECTOR_SIZE))
    {
        return STRG_FAIL;
    }

    // Hash Table Initialisation
    hash_init(&db->htable, &db->storage, &db->contact_allocator, &db->message_allocator, htable_mem, HASH_TABLE_SIZE);

    if (!sorted_array_init(&db->name_index, htable_mem, &db->storage, sa_mem))
    {
        return STRG_FAIL;
    }

    return history_init(db);
}

/**
 * @brief Add a new contact to the hash table, storage and name index
 *
 * @param db database struct pointer
 * @param name contact name
 * @param phone contact phone number
 * @retval STRG_OK if added, STRG_EXISTS if phone number already exists, else STRG_* error code
 */
STRG_RET database_contact_add(Database *db, const char *name, const char *phone)
{
    ContactBuffer contact;
    ContactBuffer removed;
    uint16_t slot;
    STRG_RET ret;

    if ((db == NULL) || !valid_str(name, MAX_NAME_LEN) || !valid_phone(phone))
    {
        return STRG_FAIL;
    }

    contact = create_contact(name, phone);

    ret = hash_insert_contact(&db->htable, &db->journal, &contact, &slot);
    if (ret != STRG_OK)
    {
        return ret;
    }

    ret = sorted_array_insert(&db->name_index, &contact.contact, slot);
    if (ret != STRG_OK)
    {
        // keep hash table and name index consistent
        hash_remove_contact(&db->htable, &db->journal, phone, &removed);
    }

    return ret;
}

/**
 * @brief Find a contact by phone number
 *
 * @param db database struct pointer
 * @param phone contact phone number
 * @param out found contact
 * @retval STRG_OK if found, STRG_EMPTY if not found, else STRG_* error code
 */
STRG_RET database_contact_get(Database *db, const char *phone, ContactBuffer *out)
{
    HashEntry *entry;
    STRG_RET ret;

    if ((db == NULL) || (out == NULL) || !valid_phone(phone))
    {
        return STRG_FAIL;
    }

    ret = find_occupied(db, phone, &entry);
    if (ret != STRG_OK)
    {
        return ret;
    }

    return read_contact(&db->storage, entry->sector, out);
}

/**
 * @brief Update the name and/or phone number of a contact
 *
 * @param db database struct pointer
 * @param phone current contact phone number
 * @param new_name new contact name
 * @param new_phone new contact phone number
 * @retval STRG_OK if updated, STRG_EMPTY if not found, STRG_EXISTS if new phone already exists
 */
STRG_RET database_contact_update(Database *db, const char *phone, const char *new_name, const char *new_phone)
{
    ContactBuffer old_contact;
    ContactBuffer new_contact;
    HashEntry *entry;
    uint16_t old_slot;
    STRG_RET ret;

    if ((db == NULL) || !valid_phone(phone) || !valid_str(new_name, MAX_NAME_LEN) || !valid_phone(new_phone))
    {
        return STRG_FAIL;
    }

    ret = find_occupied(db, phone, &entry);
    if (ret != STRG_OK)
    {
        return ret;
    }
    old_slot = slot_of(entry);

    ret = read_contact(&db->storage, entry->sector, &old_contact);
    if (ret != STRG_OK)
    {
        return ret;
    }

    new_contact = create_contact(new_name, new_phone);

    // name index compares against the stored contact, so remove before it changes
    ret = sorted_array_remove(&db->name_index, &old_contact.contact);
    if (ret != STRG_OK)
    {
        return ret;
    }

    ret = hash_update_contact(&db->htable, &db->journal, &old_contact.contact, &new_contact.contact);
    if (ret != STRG_OK)
    {
        sorted_array_insert(&db->name_index, &old_contact.contact, old_slot);
        return ret;
    }

    // a phone change moves the contact to a new hash slot
    ret = find_occupied(db, new_phone, &entry);
    if (ret != STRG_OK)
    {
        return STRG_FAIL;
    }

    return sorted_array_insert(&db->name_index, &new_contact.contact, slot_of(entry));
}

/**
 * @brief Remove a contact and its message chat
 *
 * @param db database struct pointer
 * @param phone contact phone number
 * @retval STRG_OK if removed, STRG_EMPTY if not found, else STRG_* error code
 */
STRG_RET database_contact_remove(Database *db, const char *phone)
{
    ContactBuffer contact;
    HashEntry *entry;
    uint16_t slot;
    STRG_RET ret;

    if ((db == NULL) || !valid_phone(phone))
    {
        return STRG_FAIL;
    }

    ret = find_occupied(db, phone, &entry);
    if (ret != STRG_OK)
    {
        return ret;
    }
    slot = slot_of(entry);

    ret = read_contact(&db->storage, entry->sector, &contact);
    if (ret != STRG_OK)
    {
        return ret;
    }

    // remove from name index while the contact is still readable
    ret = sorted_array_remove(&db->name_index, &contact.contact);
    if (ret != STRG_OK)
    {
        return ret;
    }

    if (!hash_remove(&db->htable, &db->journal, phone, NULL))
    {
        sorted_array_insert(&db->name_index, &contact.contact, slot);
        return STRG_FAIL;
    }

    return STRG_OK;
}

/**
 * @brief Get a range of contacts in alphabetical order
 *
 * @param db database struct pointer
 * @param start rank of the first contact
 * @param n max number of contacts to read
 * @param out_count number of contacts read
 * @param out contact array (at least n long)
 * @retval STRG_OK if read, STRG_EMPTY if start is past the last contact, else STRG_* error code
 */
STRG_RET database_contact_list(Database *db, size_t start, size_t n, size_t *out_count, ContactBuffer *out)
{
    SortedArrayEntry entry;
    size_t count;
    STRG_RET ret;

    if ((db == NULL) || (out_count == NULL) || (out == NULL))
    {
        return STRG_FAIL;
    }

    *out_count = 0;

    if (start >= db->name_index.occupancy)
    {
        return STRG_EMPTY;
    }

    for (size_t i = 0; i < n; i++)
    {
        if (!sorted_array_get_range(&db->name_index, start + i, 1, &count, &entry))
        {
            break; // end of name index
        }

        ret = read_contact(&db->storage, htable_mem[entry.hash_slot].sector, &out[i]);
        if (ret != STRG_OK)
        {
            return ret;
        }

        (*out_count)++;
    }

    return STRG_OK;
}

/**
 * @brief Find the rank of the first contact whose name is >= prefix. Use with
 * database_contact_list for search
 *
 * @param db database struct pointer
 * @param prefix name prefix
 * @param rank rank of the first contact >= prefix
 * @retval STRG_RET
 */
STRG_RET database_contact_seek(Database *db, const char *prefix, size_t *rank)
{
    if ((db == NULL) || (rank == NULL) || !valid_str(prefix, MAX_NAME_LEN))
    {
        return STRG_FAIL;
    }

    return sorted_array_seek(&db->name_index, prefix, rank);
}

/**
 * @brief Get the number of contacts in the database
 *
 * @param db database struct pointer
 * @retval number of contacts
 */
size_t database_contact_count(Database *db)
{
    if (db == NULL)
    {
        return 0;
    }

    return hash_size(&db->htable);
}

/**
 * @brief Add a message to a contacts chat and the message history. If the
 * phone number has no contact an unnamed contact is created
 *
 * @param db database struct pointer
 * @param phone phone number the message is associated with
 * @param timestamp message timestamp (must not be 0)
 * @param direction sending or receiving
 * @param str message string
 * @retval STRG_RET
 */
STRG_RET database_message_add(Database *db, const char *phone, uint32_t timestamp, bool direction, char *str)
{
    MessageBuffer message;
    MessageHistoryEntry history;
    ContactBuffer contact;
    HashEntry *entry;
    bool is_new_contact;
    STRG_RET ret;

    if ((db == NULL) || !valid_phone(phone) || (str == NULL) || (timestamp == 0))
    {
        return STRG_FAIL;
    }

    is_new_contact = (find_occupied(db, phone, &entry) != STRG_OK);

    message = create_message(timestamp, direction, str);

    if (!hash_insert_message(&db->htable, &db->journal, phone, &message))
    {
        return STRG_FAIL;
    }

    ret = find_occupied(db, phone, &entry);
    if (ret != STRG_OK)
    {
        return STRG_FAIL;
    }

    // hash_insert_message created an unnamed contact, which the name index must also hold
    if (is_new_contact)
    {
        contact = create_contact("", phone);
        ret = sorted_array_insert(&db->name_index, &contact.contact, slot_of(entry));
        if (ret != STRG_OK)
        {
            return ret;
        }
    }

    memset(&history, 0, sizeof(history));
    history.msgInd = entry->latest_msg_extent;
    history.message = message.msg;

    return message_history_add(&db->message_history, &db->journal, &db->storage, &history);
}

/**
 * @brief Read a range of messages from a contacts chat, from latest backwards
 *
 * @param db database struct pointer
 * @param phone phone number the chat is associated with
 * @param start start message (0 = latest)
 * @param n max number of messages to read
 * @param out_count number of messages read
 * @param out message array (at least n long)
 * @retval STRG_OK if read, STRG_EMPTY if no contact or chat, else STRG_* error code
 */
STRG_RET database_message_get_chat(Database *db, const char *phone, size_t start, size_t n, size_t *out_count,
        Message *out)
{
    HashEntry *entry;
    STRG_RET ret;

    if ((db == NULL) || (out_count == NULL) || (out == NULL) || !valid_phone(phone))
    {
        return STRG_FAIL;
    }

    *out_count = 0;

    ret = find_occupied(db, phone, &entry);
    if (ret != STRG_OK)
    {
        return ret;
    }

    if (entry->latest_msg_extent == UINT16_MAX)
    {
        return STRG_EMPTY;
    }

    return read_n_messages(&db->storage, entry->latest_msg_extent, start, n, out_count, out);
}

/**
 * @brief Remove a contacts entire message chat. The contact is kept
 *
 * @param db database struct pointer
 * @param phone phone number the chat is associated with
 * @retval STRG_OK if removed, STRG_EMPTY if no contact or chat, else STRG_* error code
 */
STRG_RET database_message_remove_chat(Database *db, const char *phone)
{
    MessageBuffer removed;
    HashEntry *entry;
    STRG_RET ret;

    if ((db == NULL) || !valid_phone(phone))
    {
        return STRG_FAIL;
    }

    ret = find_occupied(db, phone, &entry);
    if (ret != STRG_OK)
    {
        return ret;
    }

    if (entry->latest_msg_extent == UINT16_MAX)
    {
        return STRG_EMPTY;
    }

    if (!hash_remove_message(&db->htable, &db->journal, phone, &removed))
    {
        return STRG_FAIL;
    }

    return STRG_OK;
}

/**
 * @brief Get a range of the latest messages across all chats
 *
 * @param db database struct pointer
 * @param start start message (0 = latest)
 * @param n max number of messages to read
 * @param out_count number of messages read
 * @param out message history array (at least n long)
 * @retval STRG_RET
 */
STRG_RET database_message_recent(Database *db, size_t start, size_t n, size_t *out_count, MessageHistoryEntry *out)
{
    if ((db == NULL) || (out_count == NULL) || (out == NULL))
    {
        return STRG_FAIL;
    }

    return message_history_get_range(&db->message_history, &db->storage, start, n, out_count, out);
}

/**
 * @brief Add a call to the call history. If the phone number has no contact
 * the entry is stored with only the phone number
 *
 * @param db database struct pointer
 * @param phone phone number of the call
 * @retval STRG_RET
 */
STRG_RET database_call_add(Database *db, const char *phone)
{
    CallHistoryEntry call;
    ContactBuffer contact;
    HashEntry *entry;
    STRG_RET ret;

    if ((db == NULL) || !valid_phone(phone))
    {
        return STRG_FAIL;
    }

    memset(&call, 0, sizeof(call));

    ret = find_occupied(db, phone, &entry);
    if (ret == STRG_OK)
    {
        ret = read_contact(&db->storage, entry->sector, &contact);
        if (ret != STRG_OK)
        {
            return ret;
        }
        call.contact_index = entry->sector;
    } else {
        // unknown caller
        contact = create_contact("", phone);
        call.contact_index = UINT16_MAX;
    }
    call.contact = contact.contact;

    return call_history_add(&db->call_history, &db->journal, &db->storage, &call);
}

/**
 * @brief Get a range of the latest calls
 *
 * @param db database struct pointer
 * @param start start call (0 = latest)
 * @param n max number of calls to read
 * @param out_count number of calls read
 * @param out call history array (at least n long)
 * @retval STRG_RET
 */
STRG_RET database_call_recent(Database *db, size_t start, size_t n, size_t *out_count, CallHistoryEntry *out)
{
    if ((db == NULL) || (out_count == NULL) || (out == NULL))
    {
        return STRG_FAIL;
    }

    return call_history_get_range(&db->call_history, &db->storage, start, n, out_count, out);
}
