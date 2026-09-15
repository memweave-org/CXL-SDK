#ifndef MEMKIND_HH
#define MEMKIND_HH

#include <memkind.h>

#include "mm.h"

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE (64)
#endif

extern memkind_t memkind_pool;
extern size_t SHM_SIZE;
extern size_t SHM_TOTAL_SIZE;
extern size_t TOTAL_QUEUE_SIZE;
extern void *GLOBAL_BASE;
extern size_t QUEUE_SIZE;
extern void* QUEUE_BASE;
extern void* LOCAL_BASE;
extern void* LOCAL_BORDER;

// void *marker_malloc(memkind_t kind, size_t size);
// int marker_posix_memalign(memkind_t kind, void **memptr, size_t alignment,
//                           size_t size);
// int marker_clalign(memkind_t kind, void **memptr, size_t size);
// void marker_free(memkind_t kind, void *ptr);

// void initialize_shm();

// void init_cacheable_allocator(int machine_no);
// void destroy_memkind_fixed();

#endif
