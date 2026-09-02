# RTDBMS Project Context — STM32 Embedded Database

## 1. Project Overview

I am developing an embedded Real-Time Database Management System (RTDBMS) in C for an STM32H7-based mobile-phone-like embedded system.

The broader project is focused on implementing higher-level mobile OS functionality on a resource-constrained embedded device. The RTDBMS is intended to provide persistent storage for functionality such as:

* Contacts
* Call history
* SMS/message history
* Other phone-related persistent data

The RTDBMS runs directly on an STM32 microcontroller and uses an SD card as persistent storage.

This is NOT intended to be a conventional desktop/server database.

The architecture must account for:

* Very limited RAM
* Limited MCU processing resources
* SD-card latency
* SD-card write endurance
* Power loss during writes
* Partial sector writes
* Deterministic-ish behaviour where practical
* Static allocation where possible
* Embedded C
* No operating-system filesystem dependency for the database
* Recovery after unexpected power loss

The database therefore uses custom sector-level storage rather than relying on FATFS as the database storage abstraction.

---

# 2. Hardware / Environment

Target MCU family:

* STM32H7
* Currently developing around STM32H725/H723-class hardware

Persistent storage:

* SD card
* Sector size is 512 bytes

The database communicates with the SD card through a custom storage abstraction.

I want the database to operate on raw sectors rather than treating the SD card as a normal filesystem.

The SD card has a large number of sectors, so reading the entire card during startup is undesirable.

---

# 3. Main Database Goal

The database needs to efficiently store and retrieve structured objects from the SD card.

Currently two major persistent structures are:

## Contact

Contacts are stored inside `ContactSector`.

A `ContactSector` contains multiple contacts.

Conceptually:

```text
ContactSector
+-------------------------+
| Header                  |
| Used bitmap             |
+-------------------------+
| Contact 0               |
| Contact 1               |
| Contact 2               |
| ...                     |
| Contact N               |
+-------------------------+
```

The contact's logical ID is used to locate the contact through a hash table.

---

## Messages

Messages are stored using `MessageBlock` / message extents.

The exact message storage design is still evolving, but messages will also be stored in fixed-size SD-card regions/sectors/blocks.

The important architectural requirement is that the hash-table implementation should not care whether a location contains a Contact or a Message.

---

# 4. Storage Model

The fundamental persistent storage unit is an SD-card sector.

Conceptually:

```text
SD Card
+---------+---------+---------+---------+---------+
| Sector  | Sector  | Sector  | Sector  | Sector  |
|   0     |   1     |   2     |   3     |   4     |
+---------+---------+---------+---------+---------+
```

The database maintains its own interpretation of these sectors.

A sector may contain:

* Contact data
* Message data
* Usage bitmap
* Journal data
* Metadata
* Future database structures

The generic storage layer should therefore deal with raw sectors/bytes rather than specific database structures.

---

# 5. Storage Abstraction

I currently use a `Storage` abstraction so that the database does not need to know the exact physical storage implementation.

The Storage abstraction can represent things such as:

* SD-card storage
* Heap-backed storage for testing
* Mock storage for unit tests

The conceptual interface is:

```c
storage_read_sector(...)
storage_write_sector(...)
```

The storage layer should answer:

> "How do I read/write sector N?"

It should NOT answer:

> "What does this sector mean?"

Therefore the storage layer should not know about:

* `Contact`
* `Message`
* Hash tables
* Database semantics

---

# 6. Usage Bitmap

The database maintains a Usage Bitmap that records which sectors are currently in use.

The bitmap itself is persistent and stored on the SD card.

At startup:

```text
SD Usage Bitmap
       |
       v
Read entire bitmap
       |
       v
RAM Usage Bitmap
```

The entire bitmap is loaded into RAM.

This is intentional because checking whether a sector is used should not require an SD-card read.

For example:

```c
is_sector_used(sector)
```

should normally be a RAM operation.

---

# 7. Purpose of the Usage Bitmap

The Usage Bitmap has two major purposes.

## Allocation

It provides information about which sectors are currently allocated.

## Startup reconstruction

It allows the database to avoid scanning every SD-card sector.

Instead:

```text
Power On
   |
   v
Read Usage Bitmap
   |
   v
Find used sectors
   |
   v
Read only used sectors
```

For a large SD card this can dramatically reduce startup I/O.

---

# 8. RAM vs Persistent Usage Bitmap

The RAM Usage Bitmap should be considered the current working state.

The SD-card Usage Bitmap is the persistent representation.

During normal operation:

```text
RAM bitmap
    |
    | changes
    v
Persistent SD bitmap
```

If an already-used sector is updated:

```text
RAM bitmap = 1
SD bitmap  = 1

No bitmap modification is necessary.
```

If an unused sector becomes allocated:

```text
RAM bitmap: 0 -> 1
SD bitmap:  0 -> 1
```

If a sector is released:

```text
RAM bitmap: 1 -> 0
SD bitmap:  1 -> 0
```

These persistent bitmap changes need to participate in transactional recovery.

---

# 9. FreeList

There is also a `FreeList`.

The FreeList is responsible for finding/allocating a free sector/location.

Conceptually:

```text
Request new object
       |
       v
    FreeList
       |
       v
   sector number
```

The Usage Bitmap and FreeList have related responsibilities, and this relationship is something I am still evaluating.

I want to avoid accidentally having two independent sources of truth.

The architecture should clearly distinguish:

* Which component determines whether a sector is allocated
* Which component determines which free sector should be allocated
* How those two structures stay consistent

---

# 10. Hash Table

The database uses an in-RAM hash table for fast lookup.

The current implementation uses double hashing.

The table maps a logical ID to a physical sector/location.

Conceptually:

```text
Logical ID
    |
    v
Hash Table
    |
    v
Sector ID
```

For example:

```text
Contact ID 1234 -> Sector 500
```

The hash table itself is stored in RAM rather than being directly read from the SD card during normal operation.

The hash table is rebuilt from persistent database state during startup.

---

# 11. Hash Table Design

The hash table uses:

* Primary hash
* Secondary hash
* Double hashing for collision resolution
* `ENTRY_EMPTY`
* `ENTRY_OCCUPIED`
* `ENTRY_DELETED`
* A FreeList for allocating physical locations

The current hash entry is conceptually:

```c
typedef struct {
    uint16_t id;
    uint16_t sector;
    uint8_t state;
    ...
} HashEntry;
```

The exact structure may evolve.

The important architectural principle is:

> The hash table should only know about the relationship between a key and a location.

It should NOT know what is stored at that location.

Therefore:

```text
Hash Table knows:

    ID -> Sector

Hash Table does NOT know:

    Sector -> ContactSector
    Sector -> MessageBlock
```

---

# 12. Current Hash Table Problem

The current `hash_table.c` is too tightly coupled to Contacts.

It contains functions such as:

```text
read_contact_sector()
write_contact_sector()
write_contact()
read_contact()
remove_contact()

hash_insert_contact()
hash_find_contact()
hash_remove_contact()
```

These functions cause the hash-table module to directly depend on:

* `Contact`
* `ContactBuffer`
* `ContactSector`
* `ContactSectorBuffer`
* `Journal`
* `Storage`

This is the architecture I am currently refactoring.

---

# 13. Desired Hash Table Architecture

I want the hash table to become generic.

The core hash table should provide functionality such as:

```c
hash_init()
hash_insert()
hash_find_entry()
hash_find_sector()
hash_remove()
hash_size()
hash_clear()
```

The hash table should NOT perform SD-card I/O.

The hash table should NOT know about Contacts.

The hash table should NOT know about Messages.

The hash table should ideally NOT know about the Journal.

The hash table should primarily operate on RAM structures.

---

# 14. Desired Object Layers

I want object-specific logic separated from the hash table.

For example:

```text
contact.c / contact.h
```

should understand:

* `Contact`
* `ContactBuffer`
* `ContactSector`
* `ContactSectorBuffer`
* Contact-sector layout
* Contact position within a sector

It should provide operations such as:

```c
contact_read()
contact_write()
contact_remove()
```

Similarly:

```text
message.c / message.h
```

should understand:

* `MessageBlock`
* Message layout
* Message extents
* Message-specific persistence

It should provide operations such as:

```c
message_read()
message_write()
message_remove()
```

---

# 15. Desired Generic Architecture

The desired architecture is:

```text
                  DBMS
                   |
        +----------+----------+
        |                     |
     Contact                Message
      Layer                  Layer
        |                     |
        +----------+----------+
                   |
             Storage Layer
                   |
                   v
                SD Card
```

With the hash tables providing indexing:

```text
             Contact Hash Table
                    |
                    v
                 Sector ID
                    |
                    v
              Contact Layer
                    |
                    v
              Storage Layer
                    |
                    v
                  SD Card
```

and:

```text
             Message Hash Table
                    |
                    v
                 Sector ID
                    |
                    v
              Message Layer
                    |
                    v
              Storage Layer
                    |
                    v
                  SD Card
```

---

# 16. Important Separation of Responsibilities

I want the architecture to follow this principle:

## Hash Table

Knows:

```text
Key -> Location
```

Does NOT know:

```text
What is stored at Location
```

---

## Contact Layer

Knows:

```text
Contact -> ContactSector layout
```

Does NOT know:

```text
How the hash algorithm works
```

---

## Message Layer

Knows:

```text
Message -> MessageBlock layout
```

Does NOT know:

```text
How the hash algorithm works
```

---

## Storage Layer

Knows:

```text
Sector -> physical storage operation
```

Does NOT know:

```text
What the sector represents
```

---

## Usage Bitmap

Knows:

```text
Sector -> allocated/free state
```

---

## FreeList

Knows:

```text
Which locations can be allocated
```

---

## Journal

Knows:

```text
How to recover persistent state after an interrupted transaction
```

---

## DB / Transaction Layer

Coordinates operations involving multiple components.

For example:

```text
Insert Contact

Hash Table
    +
FreeList
    +
Contact storage
    +
Usage Bitmap
    +
Rollback Journal
```

This coordination should not be hidden inside the hash-table implementation.

---

# 17. Rollback Journal

The database has a rollback journal because SD-card operations are not inherently transactional.

A logical database operation may require multiple physical writes.

For example, inserting a new Contact may involve:

```text
1. Allocate sector
2. Update hash table in RAM
3. Write ContactSector
4. Update Usage Bitmap in RAM
5. Write Usage Bitmap to SD
6. Commit transaction
```

A power failure can occur between any of these operations.

The Journal exists to allow the database to restore the previous persistent state.

---

# 18. Important Failure Cases

The architecture needs to account for cases such as:

### Case 1

ContactSector successfully written:

```text
Contact = new
Bitmap = old
```

If the bitmap still says the sector is unused, startup reconstruction could ignore valid data.

### Case 2

Bitmap successfully written:

```text
Bitmap = new
ContactSector = old/partial
```

Startup may attempt to read invalid data.

Therefore the data sector and Usage Bitmap cannot simply be treated as independent writes.

They need transaction/recovery semantics.

---

# 19. Transaction Concept

A transaction should conceptually look like:

```text
Begin Transaction
       |
       v
Journal old persistent state
       |
       v
Perform writes
       |
       v
Update metadata
       |
       v
Commit
```

If power fails before commit:

```text
Power Failure
      |
      v
Startup
      |
      v
Journal Recovery
      |
      v
Restore previous persistent state
```

The exact transaction protocol is still being developed.

---

# 20. Startup Reconstruction

The hash tables are RAM structures and therefore disappear on power loss.

They need to be reconstructed.

The intended process is:

```text
Power On
   |
   v
Read persistent metadata
   |
   v
Read Usage Bitmap
   |
   v
Load bitmap into RAM
   |
   v
Iterate only over used sectors
   |
   v
Read sector
   |
   v
Determine sector/object type
   |
   +-----------> Contact
   |                 |
   |                 v
   |          Insert into Contact Hash Table
   |
   +-----------> Message
                     |
                     v
              Insert into Message Hash Table
```

This is one of the main reasons the Usage Bitmap exists.

---

# 21. Memory Constraints

This is an embedded system.

RAM is limited, so large dynamic data structures should be avoided where possible.

The hash table is intended to be allocated in a known RAM region / statically allocated memory on the STM32.

Host-side GoogleTest builds may use heap allocation for testing.

The production implementation should not assume an abundant heap.

---

# 22. Testing

I use GoogleTest for host-side unit testing of the C implementation.

The host build defines:

```c
HOST_BUILD
```

to enable testing-specific functionality such as:

* heap allocation
* collision statistics
* mock SD storage

The same core C implementation should ideally remain usable on the STM32 and host test environment.

---

# 23. Hash Table Performance

The hash table is intended to support approximately 10,000 entries.

I have experimented with table sizes and collision behaviour.

A prime-sized table and double hashing are being used to reduce clustering.

The exact table size is still subject to tuning, but the intended load factor is below approximately 70%, with a lower load factor being preferred for lookup performance.

---

# 24. Current Design Philosophy

The system should prefer:

* Simple data structures
* Predictable memory usage
* Minimal SD reads
* Minimal unnecessary SD writes
* Sector-aligned I/O
* RAM caching of frequently accessed metadata
* Clear ownership of responsibilities
* Recovery from power failure
* Testable modules
* Low coupling between modules

Avoid unnecessary abstraction for abstraction's sake.

This is embedded software, so abstraction must still make sense in terms of:

* RAM usage
* flash usage
* execution cost
* complexity
* reliability

---

# 25. Main Architectural Question

The major question I am currently solving is:

> How should I separate the generic hash-table/indexing functionality from the Contact/Message storage functionality while maintaining efficient sector-level access and transactional consistency?

The desired conceptual separation is:

```text
                  +----------------+
                  |    DB / Tx     |
                  |    Manager     |
                  +-------+--------+
                          |
              +-----------+-----------+
              |                       |
              v                       v
       +-------------+        +-------------+
       | Contact     |        | Message     |
       | Layer       |        | Layer       |
       +------+------+        +------+------+
              |                      |
              |                      |
              v                      v
       +-------------+        +-------------+
       | Contact     |        | Message     |
       | Hash Table  |        | Hash Table  |
       +------+------+        +------+------+
              |                      |
              +----------+-----------+
                         |
                         v
                  +-------------+
                  |  Storage    |
                  | Layer       |
                  +------+------+
                         |
                         v
                     SD Card
```

However, I am open to changing this architecture if there is a better embedded-database design.

---

# 26. What I Need Help With

When analysing or modifying this project, do not assume a conventional application/database architecture.

Keep the STM32 and SD-card constraints in mind.

In particular, I want help with:

1. Module boundaries
2. C header/source dependencies
3. Avoiding circular includes
4. Hash table abstraction
5. Storage abstraction
6. Usage Bitmap design
7. FreeList design
8. Journal/transaction design
9. Startup reconstruction
10. Power-loss recovery
11. SD-card write minimisation
12. RAM usage
13. Unit-testability
14. Clear ownership of state

When proposing an architecture, explicitly identify:

* Who owns the data
* Who modifies the data
* Who performs physical I/O
* Who controls transactions
* Which module depends on which module
* Which module should NOT depend on another module

The goal is a maintainable embedded RTDBMS rather than simply making the current code compile.

