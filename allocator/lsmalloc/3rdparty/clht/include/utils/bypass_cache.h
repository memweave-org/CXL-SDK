#ifndef BYPASS_CACHE_HH
#define BYPASS_CACHE_HH

#include <cassert>
#include <utility>

#include <iostream>

#include "utils/config.h"

// Architecture-specific includes
#ifdef __x86_64__
#include <emmintrin.h>  // For SSE2 intrinsics
#include <smmintrin.h>
#include <stdint.h>
#include <x86intrin.h>
#include <immintrin.h>
#endif

#ifdef __aarch64__
#include <arm_acle.h>
#endif

// Cache line size definition
#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE (64)
#endif

// Memory fence implementation
static inline void mfence() { 
#ifdef __x86_64__
    asm volatile("mfence" ::: "memory");
#elif defined(__aarch64__)
    asm volatile("dmb ish" ::: "memory");
#endif
}

// Cache operations with architecture-specific implementations
#ifdef __x86_64__
// x86_64 implementations
static inline void clflush(const void* data, size_t len, bool fence = true) {
    volatile char* ptr = (char*)((unsigned long)data & (~(CACHE_LINE_SIZE - 1)));
    if (fence) mfence();
    for (; ptr < (char*)data + len; ptr += CACHE_LINE_SIZE) {
#ifdef USE_CLFLUSH
        __asm__ __volatile__("clflush %0" : "+m"(*(volatile char*)ptr));
#else
        __asm__ __volatile__(".byte 0x66; clflush %0" : "+m"(*(volatile char *)ptr));
#endif
    }
    if (fence) mfence();
}

static inline void clwb(const void* data, size_t len, bool fence = true) {
    volatile char* ptr = (char*)((unsigned long)data & (~(CACHE_LINE_SIZE - 1)));
    if (fence) mfence();
    for (; ptr < (char*)data + len; ptr += CACHE_LINE_SIZE) {
#ifdef USE_CLFLUSH
        __asm__ volatile("clflush %0" : "+m"(*(volatile char*)ptr));
#elif defined(USE_CLFLUSH_OPT)
        __asm__ volatile(".byte 0x66; clflush %0" : "+m"(*(volatile char *)ptr));
#elif defined(USE_CLWB)
        __asm__ volatile(".byte 0x66; xsaveopt %0" : "+m"(*(volatile char *)(ptr)));
#endif
    }
    if (fence) mfence();
}

#elif defined(__aarch64__)
// ARM64 implementations
static inline void clflush(const void* data, size_t len, bool fence = true) {
    volatile char* ptr = (char*)((unsigned long)data & (~(CACHE_LINE_SIZE - 1)));
    if (fence) mfence();
    for (; ptr < (char*)data + len; ptr += CACHE_LINE_SIZE) {
      asm volatile("dc civac, %0" ::"r"(ptr) : "memory");
      asm volatile("dsb ish" ::: "memory");
    }
    if (fence) mfence();
}

static inline void clwb(const void* data, size_t len, bool fence = true) {
    volatile char* ptr = (char*)((unsigned long)data & (~(CACHE_LINE_SIZE - 1)));
    if (fence) mfence();
    for (; ptr < (char*)data + len; ptr += CACHE_LINE_SIZE) {
      asm volatile("dc cvac, %0" ::"r"(ptr) : "memory");
      asm volatile("dsb ish" ::: "memory");
    }
    if (fence) mfence();
}
#endif

// Non-temporal memory operations
#if 1
// TODO: every memory ordering need to be further examined if no cache coherence
// Can be substituted with memcpy or direct load/store if cache coherent

// The minimum granularity of write is 32 bit, while for read is 128 bit.

// For write: It seems that non-temporal store is faster than clflush,
// https://arxiv.org/pdf/1705.00264

// For read: Non-temporal load only support 128, 256, 512 bit, maybe we can use
// clflush/clflushopt to invalidate a cache line, and read it from DRAM again?

// Non-temporal read for 32-bit variable, the address must be 4-byte aligned
template <typename T>
static inline uint32_t READ_NT_32(T* ptr) {
  assert((uintptr_t)(ptr) % 4 == 0);
  static_assert(sizeof(T) == sizeof(uint32_t), "T must be 32-bit type");
  T* aligned_ptr = (T*)((uintptr_t)ptr & ~(uintptr_t)(0xF));
  __m128i dst = _mm_stream_load_si128((__m128i*)(aligned_ptr));
  size_t offset = ((uintptr_t)ptr & 0xF) >> 2;
  return *((uint32_t*)&dst + offset);
}

// Non-temporal write for 32-bit variable, supports both lvalue and rvalue for
// value
template <typename T, typename D>
static inline void WRITE_NT_32(T* ptr, D&& value) {
  static_assert(sizeof(T) == sizeof(uint32_t), "T must be 32-bit type");
  static_assert(sizeof(D) == sizeof(uint32_t), "D must be 32-bit type");
  _mm_stream_si32((int*)ptr, std::forward<D>(value));
}

// Non-temporal read for 64-bit variable, the address must be 8-byte aligned
template <typename T>
static inline uint64_t READ_NT_64(T* ptr) {
  assert((uintptr_t)(ptr) % 8 == 0);
  static_assert(sizeof(T) == sizeof(uint64_t), "T must be 64-bit type");
  T* aligned_ptr = (T*)((uintptr_t)ptr & ~(uintptr_t)(0xF));
  __m128i dst = _mm_stream_load_si128(
      const_cast<__m128i*>(reinterpret_cast<const __m128i*>(aligned_ptr)));
  size_t offset = ((uintptr_t)ptr & 0xF) >> 3;
  return *((uint64_t*)&dst + offset);
}

// Non-temporal write for 64-bit variable, supports both lvalue and rvalue for
// value
template <typename T, typename D>
static inline void WRITE_NT_64(T* ptr, D&& value) {
  static_assert(sizeof(T) == sizeof(uint64_t), "T must be 64-bit type");
  static_assert(sizeof(D) == sizeof(uint64_t), "D must be 64-bit type");
  _mm_stream_si64((long long*)ptr, std::forward<D>(value));
}

// Non-temporal read for 128-bit variable, the address must be 16-byte aligned
template <typename T>
static inline void READ_NT_128(T* src, __m128i& dst) {
  static_assert(sizeof(T) == sizeof(__m128i), "T must be 128-bit type");
  dst = _mm_stream_load_si128(
      const_cast<__m128i*>(reinterpret_cast<const __m128i*>(src)));
}

// Non-temporal write for 128-bit variable, supports both lvalue and rvalue for
// value
template <typename T>
static inline void WRITE_NT_128(__m128i* ptr, T&& value) {
  static_assert(sizeof(T) == sizeof(__m128i), "T must be 128-bit type");
  _mm_stream_si128(ptr, std::forward<T>(value));
}

// Non-temporal write support 256 bit and 512 bit, can be added in future

/* The size, dst, src has to be 32-bit aligned, there is some way to implement a
version where dst and src is not aligned, that is to overlap some part of the
copy, but it would be too complicated. */
static inline void memcpy_nt_write(char* dst, const char* src, size_t size) {
  assert(size % 4 == 0);
  assert((uintptr_t)(dst) % 4 == 0);
  assert((uintptr_t)(src) % 4 == 0);

  size_t i = 0;

  for (; (i < size) && ((uintptr_t)(dst + i) % 16 != 0); i += 4) {
    WRITE_NT_32((uint32_t*)(dst + i), *(uint32_t*)(src + i));
  }

  for (; i + 16 <= size; i += 16) {
    WRITE_NT_128((__m128i*)(dst + i), *(__m128i*)(src + i));
  }

  for (; i < size; i += 4) {
    WRITE_NT_32((uint32_t*)(dst + i), *(uint32_t*)(src + i));
  }
}

// The size, dst, src has to be 16-byte aligned
static inline void memcpy_nt_read(char* dst, const char* src, size_t size) {
  assert(size % 16 == 0);
  assert((uintptr_t)(dst) % 16 == 0);
  assert((uintptr_t)(src) % 16 == 0);
  size_t i = 0;
  for (; i + 16 <= size; i += 16) {
    READ_NT_128((__m128i*)(src + i), *(__m128i*)(dst + i));
  }
}
#endif

#endif  // BYPASS_CACHE_HH
