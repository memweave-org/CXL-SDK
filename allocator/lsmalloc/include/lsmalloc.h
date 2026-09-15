#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Error codes
#define LSMALLOC_SUCCESS 0
#define LSMALLOC_ERROR_NOT_INITIALIZED -1
#define LSMALLOC_ERROR_INVALID_PARAM -2
#define LSMALLOC_ERROR_OUT_OF_MEMORY -3
#define LSMALLOC_ERROR_INVALID_REF -4
#define LSMALLOC_ERROR_INTERNAL -5

#define SHMREF_SIZE_BITS 32                                // 使用32位来存储size
#define SHMREF_SIZE_MASK ((1ULL << SHMREF_SIZE_BITS) - 1)  // 低32位用于size
#define SHMREF_REF_MASK (~SHMREF_SIZE_MASK)                // 高32位用于ref

#define SHMREF_SIZE(ref) ((ref) & SHMREF_SIZE_MASK)
#define SHMREF_REF(ref) ((ref) & SHMREF_REF_MASK)

// Memory reference type
typedef uint64_t SHMRef;

// Forward declaration for C handle
struct lsmem_allocator;
typedef struct lsmem_allocator* lsmem_t;

// Memory allocator attributes
typedef struct {
    size_t total_size;         // Total size of the memory pool
    size_t alignment;          // Memory alignment requirement
    int flags;                 // Additional flags
    const char* cxl_dev_path;  // CXL device path
} lsmem_attr_t;

// Initialize allocator attributes with default values
int lsmem_attr_init(lsmem_attr_t* attr);

// Create a new memory allocator
lsmem_t lsmem_create(const lsmem_attr_t* attr);

// Destroy a memory allocator
int lsmem_destroy(lsmem_t allocator);

// Allocate memory
SHMRef lsmem_malloc(lsmem_t allocator, size_t size);

// Allocate aligned memory
SHMRef lsmem_memalign(lsmem_t allocator, size_t alignment, size_t size);

// Free memory
int lsmem_free(lsmem_t allocator, SHMRef ref);

// Read data from shared memory
void* lsmem_read(lsmem_t allocator, SHMRef ref, size_t offset, size_t size);

// Write data to shared memory
int lsmem_write(lsmem_t allocator, SHMRef ref, size_t offset, const void* data, size_t size);

// Get free space
size_t lsmem_free_space(lsmem_t allocator);

// Perform garbage collection
int lsmem_gc(lsmem_t allocator);

// Get last error
const char* lsmem_error_message(int error_code);

#ifdef __cplusplus
}
#endif
