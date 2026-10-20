#pragma once
#include <cstddef>
#include <cstdint>

// All entry points use the global, compile-time UB_API selection.
const char* ub_api_name();
void* map_ub_memory(const char* name, void* base, size_t size);
void unmap_ub_memory(void* base, size_t size);
// Create once; destroy only after both workers confirm unmap/finalize.
void create_ub_memory(const char* name, size_t size, const char* provider_host,
                      uint32_t numa_id = UINT32_MAX);
void destroy_ub_memory(const char* name);
