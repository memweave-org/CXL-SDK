#pragma once
// Benchmark-only cache backend: full-system completion, no simulated latency.
#define NO_CC
#include "utils/config.h"
#define SHM_LIB_WITH_UBSM
#define SHM_UB_SPLIT
#define BYPASS_CACHE_HH
#include <cstddef>
#include <cstdint>
#include <cstring>
inline void mfence() {
#if defined(__aarch64__)
  asm volatile("dsb sy" ::: "memory");
#elif defined(__x86_64__)
  asm volatile("mfence" ::: "memory");
#else
#error Unsupported architecture
#endif
}
inline size_t cache_line_bytes() {
#if defined(__aarch64__)
  uint64_t ctr; asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
  return 4UL << ((ctr >> 16) & 15);
#else
  return 64;
#endif
}
bool is_nc_address(const void* p, size_t n);
inline void maintain(const void* p, size_t n, bool writeback, bool fence=true) {
  if (!n || is_nc_address(p,n)) return;
  const size_t line=cache_line_bytes();
  if (fence) mfence();
  for (uintptr_t a=reinterpret_cast<uintptr_t>(p)&~(line-1), end=reinterpret_cast<uintptr_t>(p)+n; a<end; a+=line) {
#if defined(__aarch64__)
    if (writeback) asm volatile("dc cvac, %0" :: "r"(a) : "memory");
    else asm volatile("dc civac, %0" :: "r"(a) : "memory");
#else
    (void)writeback;
    asm volatile("clflush (%0)" :: "r"(a) : "memory");
#endif
  }
  if (fence) mfence();
}
inline void clwb(const void* p,size_t n,bool fence=true) { maintain(p,n,true,fence); }
inline void clflush(const void* p,size_t n,bool fence=true) { maintain(p,n,false,fence); }
inline void memcpy_nt_read(char* d,const char* s,size_t n) { clflush(s,n); std::memcpy(d,s,n); }
inline void memcpy_nt_write(char* d,const char* s,size_t n) { std::memcpy(d,s,n); clwb(d,n); }
inline thread_local bool ub_read_only_thread=false;
