# SD-DB-Dev
SD Card database development for the STM32H723
## SD Card Structure
The is a total of **200.092KB/320KB** being used in RAM_D1 (AXI SRAM), which is the largest RAM storage on the STM32H723, for hash table and FLS storage.
```
                     SD CARD
┌──────────────────────────────────────────────────────────────┐
│ Sector 0                                                     │
├──────────────────────────────────────────────────────────────┤
│ Contact Hash Table (14293 * 8B = 114.334 KB)                 │ <- LOADED INTO RAM_D1
│  • Bucket 0                                                  │
│  • Bucket 1                                                  │
│  • ...                                                       │
│  • Bucket N                                                  │
├──────────────────────────────────────────────────────────────┤
│ Contact Free List (14293 * 2B = 28.586 KB)                   │ <- LOADED INTO RAM_D1
│  • Bitmap / Stack of free extents                            │
├──────────────────────────────────────────────────────────────┤
│ Messages Free List (2 * 14293 * 2B = 57.172KB)               │ <- LOADED INTO RAM_D1
│  • Bitmap / Stack of free extents                            │
├──────────────────────────────────────────────────────────────┤
│ Contact Data (14293)                                         │
│  Contact 1                                                   │
│  Contact 2                                                   │
│  Contact 3                                                   |
|   .....                                                      │
│  Contact 14293                                               │
├──────────────────────────────────────────────────────────────┤
│ Message Data Extents (28586)                                 │
│  MessageExtent (Contact 1)                                   │
│  MessageExtent (Contact 2)                                   │
│  MessageExtent (Contact 1)                                   │
│  ...                                                         |
|  MessageExtent (Contact K)                                   │
├──────────────────────────────────────────────────────────────┤
│ Reserved                                                     │
│ (Future Expansion)                                           │
└──────────────────────────────────────────────────────────────┘
```
## Current Data Structures\
### Free List Stack (FLS)
The FLS is used to allocate memory index on the sd card to contacts and their messages in the hash table. This allows effective recycling of tombstoned contacts and messages. It works by popping index address off the stack when they are allocated and pushing them back on when they are reclaimed (freed). These will be loaded into RAM_D1 (AXI_SRAM) to track the indexes for both messages and contacts. Note memory indexes are capped at 65535 as uint16_t were used to preseve space.
```c
typedef struct  
{
    uint16_t *free_stack;
    size_t stack_top;
    size_t capacity;
    size_t used_count;
} FreeList;

```

### Hash Table
The hash table is utilised for indexing contact and message storage. The HashTable memory is an array of HashEntrys which is store in RAM_D1 (AXI SRAM). The HashEntry stores all indexing information as seen below
```c
// Hash Entry that points to SD Card sector (8B)
typedef struct
{
    uint16_t id; // Contact ID (2B)
    uint16_t sector; // SD Sector (2B)
    uint16_t latest_msg_extent; // Latest Message Extent offset (2B)
    ENTRY_STATE state;  // Entry occupation state (1B)
    uint8_t padding; // (1B)
} HashEntry;

```
And this fits into the HashTable and its metadata:
```c
typedef struct 
{
    HashEntry *htable; // In RAM hash table
    FreeList *free_stack; // List of free list stack pointers
    size_t num_elems; // amount of elements in table
    size_t size; // total space in table 
```
### Message Extents
The message extents are an organisation of linked lists where each node is an MessageBlock. Currently each MessageBlock is **4KB** and stores:
```c
// Message struct (164B)
typedef struct {
    uint16_t timestamp; // Time of message (2B)
    bool direction; // Sending or receiving (1B)
    char str [SMS_MAX_MESSAGE_LENGTH]; // Message (160B)
    uint8_t padding; // 1B
} Message;


// Message Struct (6B)
typedef struct
{
    uint16_t prev; // Previous Extent (2B)
    uint16_t msg_count; // Number of messages in the block (2B)
    EXTENT_STATE state; // Extent State (1B)
    uint8_t padding; // 1B

} MessageBlockHeader;


// Message Extent Block (4KB)
typedef struct
{
    MessageBlockHeader header; // Header
    Message messages[MESSAGE_BLOCK_CAPACITY]; // Array of chats
    uint8_t padding[MESSAGE_BLOCK_PADDING]; // Padding
} MessageBlock;
```
Below are the calculations to ensure the message block is 4KB regardless of the other structs:
```c
// Ensure a single extent is 4KB
#define EXTENT_SIZE_BYTES 4096

// Fit messages into a 4KB block of memory
#define MESSAGE_BLOCK_CAPACITY \
    ((EXTENT_SIZE_BYTES - sizeof(MessageBlockHeader)) / sizeof(Message))

// Ensure padding is accounted for
#define MESSAGE_BLOCK_PADDING \
    EXTENT_SIZE_BYTES - sizeof(MessageBlockHeader) - sizeof(Message) * MESSAGE_BLOCK_CAPACITY
```

