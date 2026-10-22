#pragma once
#include <cstddef>
#include <cstdint>

// All entry points use the global, compile-time UB_API selection.
const char* ub_api_name();
enum class UbMappingMode { Legacy, Cacheable, Noncacheable };
void* map_ub_memory(const char* name, void* base, size_t size, UbMappingMode mode = UbMappingMode::Legacy);
void unmap_ub_memory(void* base, size_t size);
// Create once; destroy only after both workers confirm unmap/finalize.
void create_ub_memory(const char* name, size_t size, const char* provider_host,
                      uint32_t numa_id = UINT32_MAX, UbMappingMode mode = UbMappingMode::Legacy);
void destroy_ub_memory(const char* name);

// Mixed-layout arenas are partitioned by process; allocations never cross slices.
void ub_configure_arena(bool atomic, void* base, size_t size);
void* ub_arena_allocate(bool atomic, size_t size, size_t alignment = 64);
size_t ub_arena_used(bool atomic);
void ub_cache_maintenance(const void*, size_t, bool writeback, bool fence);
void ub_assert_atomic_target(const void*);
