#pragma once
#include <cstddef>
#include <cstdint>

// All entry points use the global, compile-time UB_API selection.
const char* ub_api_name();
enum class UbMappingMode { Legacy, Cacheable, Noncacheable };
// UBSM Cacheable mappings invalidate this host's entire arena after ownership,
// before application access. SHM_UB_STARTUP_INVALIDATE=0 disables this only for
// controlled comparisons; clearing cached state does not clear memory contents.
void* map_ub_memory(const char* name, void* base, size_t size, UbMappingMode mode = UbMappingMode::Legacy, bool read_only = false);
void unmap_ub_memory(void* base, size_t size);
// Create once; destroy only after both workers confirm unmap/finalize.
void create_ub_memory(const char* name, size_t size, const char* provider_host,
                      uint32_t numa_id = UINT32_MAX, UbMappingMode mode = UbMappingMode::Legacy);
void destroy_ub_memory(const char* name);

// Mixed-layout arenas are partitioned by process; allocations never cross slices.
// recycle=false is reserved for readers relying on immutable addresses for the
// whole mapping lifetime. Otherwise free coalesces/reuses blocks in this slice.
void ub_configure_arena(bool atomic, void* base, size_t size, bool recycle = true);
void* ub_arena_allocate(bool atomic, size_t size, size_t alignment = 64);
// Caller must first finish the object's reader/epoch lifetime. Never free a
// published object merely because a pointer to its replacement was installed.
void ub_arena_deallocate(bool atomic, void* pointer);
size_t ub_arena_used(bool atomic);
size_t ub_arena_live(bool atomic);
void ub_cache_maintenance(const void*, size_t, bool writeback, bool fence);
void ub_assert_atomic_target(const void*);

// Optional CoW reader acquisition (SHM_UB_COW_ACQUIRE=1, off by default) for the
// split, read-only CACHE mapping. This does not replace startup invalidation.
// Objects must
// stay immutable and their addresses must not be reused within this mapping.
// The local registry is cleared on unmap. No effect on writer/NC/non-UB data.
bool ub_cache_needs_read_acquire(const void*);
void ub_acquire_immutable(const void*, void (*invalidate)(const void*));
void ub_acquire_immutable_bytes(const void*, size_t);
