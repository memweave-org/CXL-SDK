#pragma once
#include "compiler.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <algorithm> // For std::max
#include <ctime>     // For clock_gettime, CLOCK_MONOTONIC, timespec

class AtomicRequestQueue {
  using time_point = int64_t;

  static constexpr uint32_t bucket_count = 65536;
  static constexpr uintptr_t bucket_hash_mask = bucket_count - 1;
  static constexpr uint32_t CACHE_LINE_SHIFT = 6;

  static constexpr uint32_t c2m_roundtrip = 170;
  static constexpr uint32_t c2m_halftrip = c2m_roundtrip / 2;
  static constexpr uint32_t c2c_roundtrip = 250;
  static constexpr uint32_t c2c_halftrip = c2c_roundtrip / 2;

// #define USE_CLFLUSH_LATENCY
#ifdef USE_CLFLUSH_LATENCY
  static constexpr time_point cas_process_time = 204;
  static constexpr time_point store_process_time = 182;
  static constexpr time_point load_process_time = 240;
#else
  static constexpr time_point cas_process_time = 140;
  static constexpr time_point store_process_time = 140;
  static constexpr time_point load_process_time = 140;
#endif

  static uint64_t blocked_cnt; // Definition remains in .cc file

  static std::array<std::atomic<time_point>, bucket_count> req_queue; // Definition remains in .cc file

  static inline time_point get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<time_point>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
  }

  static inline void wait_until(time_point until_time) {
    while (get_time() < until_time)
      ; // Busy wait
  }

  static inline uint32_t bucket_hash(const void *addr) {
    return (reinterpret_cast<uintptr_t>(addr) >> CACHE_LINE_SHIFT) &
           bucket_hash_mask;
  }

public:
  static inline void pend_cas_req(const void *addr) {
    pend_req<cas_process_time>(addr);
  }
  static inline void pend_load_req(const void *addr) {
    pend_req<load_process_time>(addr);
  }
  static inline void pend_store_req(const void *addr) {
    pend_req<store_process_time>(addr);
  }

private:
  template <time_point ProcessTime>
  static inline void pend_req(const void *addr) {
    uint32_t bidx = bucket_hash(addr);
    time_point cur = get_time();

    // After halftrip, the request arrives at DRAM
    cur += c2c_halftrip;

    time_point queue_finish_point =
        req_queue[bidx].load(std::memory_order_relaxed);

    bool ret = false;
    time_point until_time;

    // Wait for the request to finish processing
    while (!ret) {
      until_time = std::max(queue_finish_point, cur) + ProcessTime;

      ret = req_queue[bidx].compare_exchange_weak(
          queue_finish_point, until_time, std::memory_order_release,
          std::memory_order_acquire);
    }

    // After processing and a halftrip, the request finished.
    wait_until(until_time + c2c_halftrip);
  }
};

template <typename T> class nt {
public:
  nt(T initialValue = T()) { store(initialValue); }

  nt(const nt &other) { store(other.load()); }

  ~nt() {}

  nt<T> &operator++() {
    fetch_add(1);
    return *this;
  }

  nt<T> operator++(int) {
    nt<T> temp = *this;
    ++value;
    return temp;
  }

  nt<T> &operator=(const T &newValue) {
    value = newValue;
    return *this;
  }

  operator T() const { return load(); }

  T load(std::memory_order __m = std::memory_order_seq_cst,
         bool nt = true) const {
    if (nt) {
      AtomicRequestQueue::pend_load_req(const_cast<const void*>(static_cast<const volatile void*>(&value)));
    }
    T ret = __atomic_load_n(&value, (int)__m);
    return ret;
  }

  void store(T newValue, std::memory_order __m = std::memory_order_seq_cst,
             bool nt = true) {
    if (nt) {
      AtomicRequestQueue::pend_store_req(const_cast<const void*>(static_cast<const volatile void*>(&value)));
    }
    __atomic_store_n(&value, newValue, (int)__m);
  }

  void flush() { AtomicRequestQueue::pend_store_req(&value); }

  T fetch_add(T i, bool nt = true) {
    T old_value;
    old_value = __atomic_fetch_add(&value, i, __ATOMIC_SEQ_CST);
    if (nt) {
      AtomicRequestQueue::pend_cas_req(const_cast<const void*>(static_cast<const volatile void*>(&value)));
    }
    return old_value;
  }

  T fetch_sub(T i, bool nt = true) {
    T old_value;
    old_value = __atomic_fetch_sub(&value, i, __ATOMIC_SEQ_CST);
    if (nt) {
      AtomicRequestQueue::pend_cas_req(const_cast<const void*>(static_cast<const volatile void*>(&value)));
    }
    return old_value;
  }

  T fetch_xor(T i, bool nt = true) {
    T old_value;
    old_value = __atomic_fetch_xor(&value, i, __ATOMIC_SEQ_CST);
    if (nt) {
      AtomicRequestQueue::pend_cas_req(const_cast<const void*>(static_cast<const volatile void*>(&value)));
    }
    return old_value;
  }

  bool
  compare_exchange_strong(T &expected, T desired,
                          std::memory_order __m = std::memory_order_seq_cst,
                          bool nt = true) {

    bool equal =
        __atomic_compare_exchange_n(&value, &expected, desired, 0, (int)__m,
                                    (int)std::__cmpexch_failure_order(__m));
    if (nt) {
      AtomicRequestQueue::pend_cas_req(const_cast<const void*>(static_cast<const volatile void*>(&value)));
    }
    return equal;
  }

private:
  T value;
};

template <typename T> class aligned_nt {
public:
  aligned_nt(T initialValue = T()) { store(initialValue); }

  aligned_nt(const aligned_nt &other) { store(other.load()); }

  ~aligned_nt() {}

  aligned_nt<T> &operator++() {
    ++value;
    return *this;
  }

  aligned_nt<T> operator++(int) {
    aligned_nt<T> temp = *this;
    ++value;
    return temp;
  }

  aligned_nt<T> &operator=(const T &newValue) {
    value = newValue;
    return *this;
  }

  operator T() const { return load(); }

  T load(std::memory_order __m = std::memory_order_seq_cst,
         bool nt = true) const {
    if (nt) {
      AtomicRequestQueue::pend_load_req(const_cast<const void*>(static_cast<const volatile void*>(&value)));
    }
    T ret = __atomic_load_n(&value, (int)__m);
    return ret;
  }

  void store(T newValue, std::memory_order __m = std::memory_order_seq_cst,
             bool nt = true) {
    __atomic_store_n(&value, newValue, (int)__m);
    if (nt) {
      AtomicRequestQueue::pend_store_req(const_cast<const void*>(static_cast<const volatile void*>(&value)));
    }
  }

  T fetch_add(T i, bool nt = true) {
    T old_value;
    old_value = __atomic_add_fetch(&value, i, __ATOMIC_SEQ_CST);
    if (nt) {
      AtomicRequestQueue::pend_cas_req(const_cast<const void*>(static_cast<const volatile void*>(&value)));
    }
    return old_value;
  }

  T fetch_sub(T i, bool nt = true) {
    T old_value;
    old_value = __atomic_fetch_sub(&value, i, __ATOMIC_SEQ_CST);
    if (nt) {
      AtomicRequestQueue::pend_cas_req(const_cast<const void*>(static_cast<const volatile void*>(&value)));
    }
    return old_value;
  }

  T fetch_xor(T i, bool nt = true) {
    T old_value;
    old_value = __atomic_fetch_xor(&value, i, __ATOMIC_SEQ_CST);
    if (nt) {
      AtomicRequestQueue::pend_cas_req(const_cast<const void*>(static_cast<const volatile void*>(&value)));
    }
    return old_value;
  }

  bool
  compare_exchange_strong(T &expected, T desired,
                          std::memory_order __m = std::memory_order_seq_cst,
                          bool nt = true) {
    bool equal =
        __atomic_compare_exchange_n(&value, &expected, desired, 0, (int)__m,
                                    (int)std::__cmpexch_failure_order(__m));
    if (nt) {
      AtomicRequestQueue::pend_cas_req(const_cast<const void*>(static_cast<const volatile void*>(&value)));
    }
    return equal;
  }

private:
  alignas(CACHE_LINE_SIZE) T value;
};