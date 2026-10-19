#pragma once
#include <atomic>
#include <stdexcept>
#include "utils/bypass_cache.h"

namespace shm_platform {
inline void pause() {
#if defined(__x86_64__) || defined(__i386__)
  asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#else
#error Unsupported CPU architecture
#endif
}
inline void full_fence() { std::atomic_thread_fence(std::memory_order_seq_cst); }
inline void store_fence() {
#if defined(__x86_64__) || defined(__i386__)
  asm volatile("sfence" ::: "memory");
#else
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}
}

namespace shm_platform {
// AArch64's exclusive pair must complete a store-exclusive even on a mismatch:
// LDAXP alone does not guarantee an atomic 128-bit snapshot. No process-local
// libatomic lock is used, so normal shared mappings work across processes.
inline unsigned __int128 compare_exchange_128(void* address,
    unsigned __int128 expected, unsigned __int128 desired) {
#if defined(SHM_DISABLE_CAS128)
  throw std::runtime_error("128-bit CAS is disabled for this benchmark (unsupported on UB)");
#elif defined(__aarch64__)
  uint64_t lo, hi;
  uint32_t retry;
  asm volatile(
      "dmb ish\n"
      "1: ldaxp %0, %1, [%3]\n"
      "cmp %0, %4\n"
      "ccmp %1, %5, #0, eq\n"
      "b.ne 2f\n"
      "stlxp %w2, %6, %7, [%3]\n"
      "cbnz %w2, 1b\n"
      "b 3f\n"
      "2: stlxp %w2, %0, %1, [%3]\n"
      "cbnz %w2, 1b\n"
      "3: dmb ish\n"
      : "=&r"(lo), "=&r"(hi), "=&r"(retry)
      : "r"(address), "r"(uint64_t(expected)), "r"(uint64_t(expected >> 64)),
        "r"(uint64_t(desired)), "r"(uint64_t(desired >> 64))
      : "cc", "memory");
  return (static_cast<unsigned __int128>(hi) << 64) | lo;
#else
  return __sync_val_compare_and_swap(static_cast<unsigned __int128*>(address), expected, desired);
#endif
}
}

namespace shm_platform {
// Hardware ticks, not a portable CPU-cycle or nanosecond unit.
inline uint64_t ticks() {
#if defined(__aarch64__)
  uint64_t value;
  asm volatile("isb; mrs %0, cntvct_el0" : "=r"(value) :: "memory");
  return value;
#else
  uint32_t lo,hi;
  asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return (uint64_t(hi)<<32)|lo;
#endif
}
}
