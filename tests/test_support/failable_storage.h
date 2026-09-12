#ifndef TEST_SUPPORT_FAILABLE_STORAGE_H
#define TEST_SUPPORT_FAILABLE_STORAGE_H

// Shared test helper: wraps heap_storage.c's real read/write logic but lets
// a test make the Nth read or Nth write (per direction) fail on demand, to
// exercise error/rollback paths that heap_storage itself never takes
// (it only fails on out-of-range access).
//
// Usage:
//   FailableStorageCtx ctx;
//   ASSERT_TRUE(FailableStorage_Init(&ctx, memory, SECTOR_SIZE, sector_count));
//   Storage storage = FailableStorage_Make(&ctx);
//   ctx.fail_after_write = 2; // the 2nd write_block call will fail; others succeed
//   ...

extern "C"
{
#include "heap_storage.h"
#include "storage.h"

// heap_storage.c defines and exports these (used via the `heap_storage`
// global's function pointers) but heap_storage.h only declares the
// single-block read/write/capacity functions. Declare the rest here so
// this wrapper can call through to the real multiblock logic too.
bool HeapStorage_ReadMultiBlock(void *context, uint32_t index, size_t readNum, uint8_t *out);
bool HeapStorage_WriteMultiBlock(void *context, uint32_t index, size_t writeNum, uint8_t *in);
}

struct FailableStorageCtx
{
    HeapStorageContext heap{};

    // -1 (default) = never fail. Otherwise, the call whose 1-based count
    // equals this value fails (returns false) instead of touching storage;
    // every other call behaves normally. Reset with a fresh assignment.
    int fail_after_read = -1;
    int fail_after_write = -1;

    int read_calls = 0;
    int write_calls = 0;
};

inline bool FailableStorage_Init(FailableStorageCtx *ctx, uint8_t *memory, uint32_t block_size,
                                  uint32_t capacity_blocks)
{
    ctx->fail_after_read = -1;
    ctx->fail_after_write = -1;
    ctx->read_calls = 0;
    ctx->write_calls = 0;
    return HeapStorage_Init(&ctx->heap, memory, block_size, capacity_blocks);
}

inline bool FailableStorage_ReadBlock(void *context, uint32_t index, uint8_t *out)
{
    FailableStorageCtx *ctx = static_cast<FailableStorageCtx *>(context);
    ctx->read_calls++;
    if (ctx->fail_after_read == ctx->read_calls)
    {
        return false;
    }
    return HeapStorage_ReadBlock(&ctx->heap, index, out);
}

inline bool FailableStorage_ReadMultiBlock(void *context, uint32_t index, size_t readNum, uint8_t *out)
{
    FailableStorageCtx *ctx = static_cast<FailableStorageCtx *>(context);
    ctx->read_calls++;
    if (ctx->fail_after_read == ctx->read_calls)
    {
        return false;
    }
    return HeapStorage_ReadMultiBlock(&ctx->heap, index, readNum, out);
}

inline bool FailableStorage_WriteBlock(void *context, uint32_t index, uint8_t *in)
{
    FailableStorageCtx *ctx = static_cast<FailableStorageCtx *>(context);
    ctx->write_calls++;
    if (ctx->fail_after_write == ctx->write_calls)
    {
        return false;
    }
    return HeapStorage_WriteBlock(&ctx->heap, index, in);
}

inline bool FailableStorage_WriteMultiBlock(void *context, uint32_t index, size_t writeNum, uint8_t *in)
{
    FailableStorageCtx *ctx = static_cast<FailableStorageCtx *>(context);
    ctx->write_calls++;
    if (ctx->fail_after_write == ctx->write_calls)
    {
        return false;
    }
    return HeapStorage_WriteMultiBlock(&ctx->heap, index, writeNum, in);
}

inline uint32_t FailableStorage_Capacity(void *context)
{
    FailableStorageCtx *ctx = static_cast<FailableStorageCtx *>(context);
    return HeapStorage_Capacity(&ctx->heap);
}

inline Storage FailableStorage_Make(FailableStorageCtx *ctx)
{
    Storage storage{};
    storage.context = ctx;
    storage.read_block = FailableStorage_ReadBlock;
    storage.read_multiblock = FailableStorage_ReadMultiBlock;
    storage.write_block = FailableStorage_WriteBlock;
    storage.write_multiblock = FailableStorage_WriteMultiBlock;
    storage.capacity = FailableStorage_Capacity;
    return storage;
}

#endif // TEST_SUPPORT_FAILABLE_STORAGE_H
