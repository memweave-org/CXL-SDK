#pragma once
// Test backend only. No implementation is linked into a normal/vendor build.
#include <cstddef>
void* local_ncc_map(int backing_fd, void* address, size_t bytes, int prot, int flags);
void local_ncc_unmap(void* address, size_t bytes);
void local_ncc_maintain(const void* address, size_t bytes, bool writeback);
// Inserted at ordinary loads by the debug-only LLVM pass. A cold cacheline is
// fetched once; a valid cacheline is never refreshed by another host's writeback.
extern "C" void local_ncc_read(const void* address, size_t bytes);
extern "C" void local_ncc_write(void* address, size_t bytes);
extern "C" void local_ncc_prefetch(const void* address);
extern "C" void local_ncc_atomic(const void* address, size_t bytes);
// Prints local/backing bytes without repairing the offending data, then aborts.
[[noreturn]] void local_ncc_bad_pointer(const void* node, const void* field, const char* name);
