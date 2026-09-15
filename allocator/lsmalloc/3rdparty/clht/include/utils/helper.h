#pragma once

#include "config.h"
#include "shm/mempool.h"

#ifdef USE_CXL // Allocate C++ Class on Shared Memroy

#define NEW_CLASS_ON_SHM                                                       \
  void *operator new(size_t size) {                                            \
    void *ptr;                                                                 \
    if (cacheable.clalign(&ptr, size) != 0) {                                  \
      throw std::bad_alloc();                                                  \
    }                                                                          \
    return ptr;                                                                \
  }                                                                            \
                                                                               \
  void operator delete(void *ptr) noexcept { cacheable.free(ptr); }

#else // Do nothing

#define NEW_CLASS_ON_SHM

#endif

#define ALLOC_AND_CONSTRUCT(type, allocator, ...)                              \
  ({                                                                           \
    void *__tmp_addr = (allocator)(sizeof(type));                              \
    new (__tmp_addr) type(__VA_ARGS__);                                        \
  })

#define DESTROY_AND_DEALLOC(obj, type, allocator)                              \
  do {                                                                         \
    if (obj) {                                                                 \
      obj->~type();                                                            \
      (allocator)(obj);                                                        \
      obj = nullptr;                                                           \
    }                                                                          \
  } while (0)
