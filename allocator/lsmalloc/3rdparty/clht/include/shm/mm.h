#pragma once

#include <cstdarg>
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

class CXLSystemMemoryMmapper : public SystemMemoryMmapper {
public:
  CXLSystemMemoryMmapper(const char *path, int num_machines, int machine_no);
  void *per_machine_base;
  size_t per_machine_size;
  int machine_no;
  int num_machines;
  void allocate(void *base, size_t size) override;
  void *get_mempool_base() override { return per_machine_base; }
  size_t get_mempool_size() override { return per_machine_size; }
};

class MemoryManager {
public:
  MemoryManager();
  MemoryManager(SystemMemoryMmapper *allocator, void *base, size_t size);
  MemoryManager(const MemoryManager &other) = delete;
  MemoryManager(MemoryManager &&other) noexcept;
  MemoryManager &operator=(const MemoryManager &other) = delete;
  MemoryManager &operator=(MemoryManager &&other) noexcept;
  ~MemoryManager();

  void *malloc(size_t size);
  int posix_memalign(void **memptr, size_t alignment, size_t size);
  int clalign(void **memptr, size_t size);
  void free(void *ptr);

  memkind_t memkind_pool;
private:
  
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
    return static_cast<T *>(cacheable.malloc(n * sizeof(T)));
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

void init_uncacheable_allocator();
