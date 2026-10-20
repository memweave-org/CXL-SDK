#pragma once
#include "ubs_mem_def.h"
#include <cstddef>
#include <sys/types.h>
extern "C" {
int ubsmem_init_attributes(ubsmem_options_t*);
int ubsmem_initialize(const ubsmem_options_t*);
int ubsmem_finalize();
int ubsmem_shmem_allocate_with_provider(const ubs_mem_provider_t*, const char*, size_t, mode_t, uint64_t);
int ubsmem_shmem_deallocate(const char*);
int ubsmem_shmem_map(void*, size_t, int, int, const char*, off_t, void**);
int ubsmem_shmem_unmap(void*, size_t);
}
