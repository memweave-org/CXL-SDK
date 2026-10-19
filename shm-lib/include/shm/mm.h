#pragma once

#include <cstdarg>
#include <cstdint>
#include <limits.h>
#include <limits>
#include <memkind.h>

#include <cstdlib>
#include <cstring>
#include <new>

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE (64)
#endif

class SystemMemoryMmapper {
public:
  char path[PATH_MAX];
  int fd;
  size_t mmap_size;
  void *mmap_base;
  SystemMemoryMmapper() : fd(-1), mmap_base(nullptr) {}
  SystemMemoryMmapper(const char *path);
  virtual void allocate(void *base, size_t size);
  virtual void deallocate();
  virtual void *get_mmap_base() { return mmap_base; }
  virtual size_t get_mmap_size() { return mmap_size; }
  virtual void *get_mempool_base() { return mmap_base; }
  virtual size_t get_mempool_size() { return mmap_size; }
  virtual ~SystemMemoryMmapper();
};

void *map_ub_memory(const char *name, void *base, size_t size);
void unmap_ub_memory(void *base, size_t size);

class CXLSystemMemoryMmapper : public SystemMemoryMmapper {
public:
  CXLSystemMemoryMmapper() = default;
  CXLSystemMemoryMmapper(const char *path, int num_machines, int machine_no);
  void *per_machine_base;
  size_t per_machine_size;
  int machine_no;
  int num_machines;
  void allocate(void *base, size_t size) override;
  void *get_mempool_base() override { return per_machine_base; }
  size_t get_mempool_size() override { return per_machine_size; }
};

/** Tag type for constructing cacheable backed by cxlalloc-static. */
struct CxlallocCacheableTag {};

enum class CacheableAllocatorBackend {
  Memkind = 0,
  Cxlalloc = 1,
};

struct CacheableInitParams {
  CacheableAllocatorBackend backend;
  /** True when INI mem_type is "cxl" (per-machine mmap slice for memkind). */
  bool mem_type_cxl;
  const char *device_path;
  void *mmap_base;
  size_t size_bytes;
  int worker_machine_count;
  int worker_machine_id;
  /** Total allocator threads (cxlalloc); includes main thread id 0. */
  uint16_t thread_count;
  int8_t cxlalloc_heap_numa;
};

class MemoryManager {
public:
  MemoryManager();
  MemoryManager(SystemMemoryMmapper *allocator, void *base, size_t size);
  explicit MemoryManager(CxlallocCacheableTag, size_t reported_size_bytes);
  MemoryManager(const MemoryManager &other) = delete;
  MemoryManager(MemoryManager &&other) noexcept;
  MemoryManager &operator=(const MemoryManager &other) = delete;
  MemoryManager &operator=(MemoryManager &&other) noexcept;
  ~MemoryManager();

  void *malloc(size_t size);
  int posix_memalign(void **memptr, size_t alignment, size_t size);
  int clalign(void **memptr, size_t size);
  void free(void *ptr);

  void require_shared_allocations(bool retain_until_exit = false) {
    strict_shared_ = true;
    retain_shared_ = retain_until_exit;
  }

  bool is_cxlalloc_backend() const {
    return backend_ == CacheableAllocatorBackend::Cxlalloc;
  }

  memkind_t memkind_pool;
private:
  bool strict_shared_ = false;
  bool retain_shared_ = false;
  bool owns_memkind_pointer(const void *ptr) const;

  CacheableAllocatorBackend backend_;
  void *base;
  size_t size;
  SystemMemoryMmapper *allocator;
};

extern MemoryManager cacheable;
#ifdef ENABLE_UNCACHE_MEM
extern MemoryManager uncacheable;
#endif

template <typename T> class CXLAllocator {
public:
  using value_type = T;
  CXLAllocator() = default;

  template <typename U> CXLAllocator(const CXLAllocator<U> &) {}

  T *allocate(size_t n) {
    if (n > std::numeric_limits<size_t>::max() / sizeof(T)) {
      throw std::bad_alloc();
    }
    T *ptr = static_cast<T *>(cacheable.malloc(n * sizeof(T)));
    if (ptr == nullptr) {
      throw std::bad_alloc();
    }
    return ptr;
  }

  void deallocate(T *p, size_t n) { cacheable.free(p); }
};

template <typename T, typename U>
bool operator==(const CXLAllocator<T> &, const CXLAllocator<U> &) {
  return true;
}

template <typename T, typename U>
bool operator!=(const CXLAllocator<T> &, const CXLAllocator<U> &) {
  return false;
}

void init_cacheable_allocator(const char *shm_path, void *base, size_t size);

void init_cxl_cacheable_allocator(const char *shm_path, void *base,
                                  size_t size);

/** Single entry: memkind vs cxlalloc, INI-driven; keeps global `cacheable` API. */
void init_cacheable_allocator_unified(const CacheableInitParams &params);

void init_uncacheable_allocator(const char *shm_path, void *base, size_t size);
