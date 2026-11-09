#pragma once

#include <atomic>
#include <type_traits>
#include "utils/bypass_cache.h"

// Hardware UB operations on the supplied address. Allocation/mapping is owned by
// the caller (shared_atomic_memory() selects NC in the split layout). There is
// no shadow value, request queue, timing model or software-lock fallback here.
namespace ub_atomic {
inline void fence() {
#if defined(__aarch64__)
  asm volatile("dmb sy" ::: "memory");
#else
  mfence();
#endif
}
template<class T> inline void check(const T* address) {
  static_assert(std::is_integral<T>::value || std::is_pointer<T>::value,
                "UB atomics require an integral or pointer value");
  static_assert(sizeof(T) <= 8 && __atomic_always_lock_free(sizeof(T), nullptr),
                "UB atomics require native lock-free operations of at most 64 bits");
#ifdef SHM_UB_SPLIT
  // Process-local temporaries are valid; shared CACHE targets are not.
  ub_assert_atomic_target(address);
#endif
}

// UB mappings can lie outside the CPU's inner-shareable domain. Use the
// platform's full-system fence in addition to the compiler atomic ordering.
inline void before(std::memory_order order) {
  if (order == std::memory_order_release || order == std::memory_order_acq_rel ||
      order == std::memory_order_seq_cst) fence();
}
inline void after(std::memory_order order) {
  if (order == std::memory_order_consume || order == std::memory_order_acquire ||
      order == std::memory_order_acq_rel || order == std::memory_order_seq_cst) fence();
}
inline std::memory_order failure_order(std::memory_order order) {
  return order == std::memory_order_acq_rel ? std::memory_order_acquire :
         order == std::memory_order_release ? std::memory_order_relaxed : order;
}

template<class T> inline T load(const T* address, std::memory_order order) {
  check(address);
#if defined(__aarch64__)
  // Native atomic loads provide the requested ordering without extra fences.
  return __atomic_load_n(address, int(order));
#else
  before(order);
  T value = __atomic_load_n(address, int(order));
  after(order);
  return value;
#endif
}
template<class T> inline void store(T* address, T value, std::memory_order order) {
  check(address);
  before(order);
  __atomic_store_n(address, value, int(order));
  after(order);
}
template<class T> inline bool compare_exchange(T* address, T& expected, T desired,
                                               std::memory_order order) {
  check(address);
  before(order);
  bool equal = __atomic_compare_exchange_n(address, &expected, desired, false,
                                           int(order), int(failure_order(order)));
  after(equal ? order : failure_order(order));
  return equal;
}
template<class T> inline T fetch_add(T* address, T value) {
  check(address);
  before(std::memory_order_seq_cst);
  // nt<T*> historically takes a byte offset encoded as T*, unlike std::atomic<T*>.
  T old;
  if constexpr (std::is_pointer<T>::value)
    old = __atomic_fetch_add(address, reinterpret_cast<uintptr_t>(value), __ATOMIC_SEQ_CST);
  else
    old = __atomic_fetch_add(address, value, __ATOMIC_SEQ_CST);
  after(std::memory_order_seq_cst);
  return old;
}
template<class T> inline T fetch_sub(T* address, T value) {
  check(address);
  before(std::memory_order_seq_cst);
  T old;
  if constexpr (std::is_pointer<T>::value)
    old = __atomic_fetch_sub(address, reinterpret_cast<uintptr_t>(value), __ATOMIC_SEQ_CST);
  else
    old = __atomic_fetch_sub(address, value, __ATOMIC_SEQ_CST);
  after(std::memory_order_seq_cst);
  return old;
}
template<class T> inline T fetch_xor(T* address, T value) {
  check(address);
  before(std::memory_order_seq_cst);
  T old = __atomic_fetch_xor(address, value, __ATOMIC_SEQ_CST);
  after(std::memory_order_seq_cst);
  return old;
}
template<class T> inline void flush(const T* address) {
  check(address);
  // NC has no cache line to write back; only order the accesses.
  fence();
}
} // namespace ub_atomic
