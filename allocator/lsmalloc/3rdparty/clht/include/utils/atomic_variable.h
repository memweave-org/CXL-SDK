#pragma once

#include <cstdint>
#include <execinfo.h>

#include <atomic>
#include <vector>

#include "bypass_cache.h"
#include "compiler.h"
#include "config.h"
#include "shm/mm.h"
#include "utils/sim_id.h"
#include "utils/timing.h"

#ifdef QUEUE_SIM_CAS
#include "atomic_queued_variable.h"
#else
#ifdef SNIPER
#include "utils/sim_api.h"
#include <type_traits>

template <typename T>
uint64_t convert_to_uint64(
    T val,
    typename std::enable_if<!std::is_pointer<T>::value>::type * = nullptr) {
  return static_cast<uint64_t>(val);
}

template <typename T>
uint64_t convert_to_uint64(
    T val,
    typename std::enable_if<std::is_pointer<T>::value>::type * = nullptr) {
  return reinterpret_cast<uint64_t>(val);
}
#endif

using namespace std;

#ifdef STAT_NT_POINTER
class Statistic {
public:
  static int atomic_cas_counter;
  static int atomic_load_counter;
};
#endif

#ifndef USE_DISAGR_CAS
#ifdef NT_SIM
// #define DEFAULT_SIM_MACHINE_COUNT (4)
template <typename T> class nt {
public:
  nt(T init = T()) {
    // cache_value=new T[SimThreadInfo::worker_machine_count]();
    // cache_available = new char[SimThreadInfo::worker_machine_count]();
    if (SimThreadInfo::worker_machine_count == 0) {
      cache_value.resize(4);
      cache_available.resize(4);
      store(init);
    } else {
      cache_value.resize(SimThreadInfo::worker_machine_count);
      cache_available.resize(SimThreadInfo::worker_machine_count);
      store(init);
    }
  }

  nt(const nt &other) {
    // cache_value = new T[SimThreadInfo::worker_machine_count]();
    // cache_available = new char[SimThreadInfo::worker_machine_count]();
    cache_value.resize(SimThreadInfo::worker_machine_count);
    cache_available.resize(SimThreadInfo::worker_machine_count);
    // cache_value[SimThreadInfo::worker_machine_id] = other.load();
    // cache_available[SimThreadInfo::worker_machine_id] = true;
    store(other.load());
  }

  // ~nt() {
  //   delete[] cache_value;
  //   delete[] cache_available;
  // }

  nt<T> &operator++() {
    ++value;
    return *this;
  }

  nt<T> operator++(int) {
    nt<T> temp = *this;
    ++value;
    return temp;
  }

  nt<T> &operator=(const T &newValue) {
    store(newValue);
    return *this;
  }

  nt<T> &operator=(const nt<T> &other) {
    store(other.load());
    return *this;
  }

  operator T() const { return load(); }

  T load(memory_order __m = memory_order_seq_cst, bool nt = true) const {
    if (!cache_available[SimThreadInfo::worker_machine_id]) [[unlikely]] {
      nt = true;
    }
    if (nt) {
      const_cast<T &>(cache_value[SimThreadInfo::worker_machine_id]) =
          __atomic_load_n(&value, (int)__m);
      const_cast<char &>(cache_available[SimThreadInfo::worker_machine_id]) =
          true;
    }
    return __atomic_load_n(&cache_value[SimThreadInfo::worker_machine_id],
                           (int)__m);
  }

  void store(T newValue, memory_order __m = memory_order_seq_cst,
             bool nt = true) {
    T *addr;
    if (nt) {
      addr = &value;
      __atomic_store_n(&cache_value[SimThreadInfo::worker_machine_id], newValue,
                       (int)__m);
      cache_available[SimThreadInfo::worker_machine_id] = true;
    } else {
      addr = &cache_value[SimThreadInfo::worker_machine_id];
    }
    __atomic_store_n(addr, newValue, (int)__m);
  }

  void flush() { cache_available[SimThreadInfo::worker_machine_id] = false; }

  void write_back() {
    if (cache_available[SimThreadInfo::worker_machine_id]) {
      value = cache_value[SimThreadInfo::worker_machine_id];
    }
  }

  T fetch_add(T i, memory_order __m = memory_order_seq_cst, bool nt = true) {
    T *addr;
    T result;
    if (!cache_available[SimThreadInfo::worker_machine_id]) [[unlikely]] {
      nt = true;
    }
    if (nt) {
      addr = &value;
      result = __atomic_add_fetch(addr, i, (int)__m);
      __atomic_store_n(&cache_value[SimThreadInfo::worker_machine_id], result,
                       (int)__m);
      result -= i;
      const_cast<char &>(cache_available[SimThreadInfo::worker_machine_id]) =
          true;
    } else {
      addr = &cache_value[SimThreadInfo::worker_machine_id];
      result = __atomic_fetch_add(addr, i, (int)__m);
    }
    return result;
  }

  T fetch_sub(T i, memory_order __m = memory_order_seq_cst, bool nt = true) {
    T *addr;
    T result;
    if (!cache_available[SimThreadInfo::worker_machine_id]) [[unlikely]] {
      nt = true;
    }
    if (nt) {
      addr = &value;
      result = __atomic_sub_fetch(addr, i, (int)__m);
      __atomic_store_n(&cache_value[SimThreadInfo::worker_machine_id], result,
                       (int)__m);
      result += (uint64_t)i;
      const_cast<char &>(cache_available[SimThreadInfo::worker_machine_id]) =
          true;
    } else {
      addr = &cache_value[SimThreadInfo::worker_machine_id];
      result = __atomic_fetch_sub(addr, i, (int)__m);
    }
    return result;
  }

  T fetch_xor(T i, memory_order __m = memory_order_seq_cst, bool nt = true) {
    T *addr;
    T result;
    if (!cache_available[SimThreadInfo::worker_machine_id]) [[unlikely]] {
      nt = true;
    }
    if (nt) {
      addr = &value;
      result = __atomic_xor_fetch(addr, i, (int)__m);
      __atomic_store_n(&cache_value[SimThreadInfo::worker_machine_id], result,
                       (int)__m);
      result ^= i;
      const_cast<char &>(cache_available[SimThreadInfo::worker_machine_id]) =
          true;
    } else {
      addr = &cache_value[SimThreadInfo::worker_machine_id];
      result = __atomic_fetch_xor(addr, i, (int)__m);
    }
    return result;
  }

  bool compare_exchange_strong(T &expected, T desired,
                               memory_order __m = memory_order_seq_cst,
                               bool nt = true) {
    if (!cache_available[SimThreadInfo::worker_machine_id]) [[unlikely]] {
      nt = true;
    }
    if (nt) {
      bool equal =
          __atomic_compare_exchange_n(&value, &expected, desired, 0, (int)__m,
                                      (int)__cmpexch_failure_order(__m));
      if (equal) {
        __atomic_store_n(&cache_value[SimThreadInfo::worker_machine_id],
                         desired, (int)__m);
        const_cast<char &>(cache_available[SimThreadInfo::worker_machine_id]) =
            true;
      }
      return equal;
    } else {
      return __atomic_compare_exchange_n(
          &cache_value[SimThreadInfo::worker_machine_id], &expected, desired, 0,
          (int)__m, (int)__cmpexch_failure_order(__m));
    }
  }

private:
  T value;
  // T cache_value[N];
  // bool cache_available[N];
  // T* cache_value;
  // char* cache_available;
  mutable std::vector<T> cache_value;
  mutable std::vector<char> cache_available;
};
#else

template <typename T> class nt {
public:
  nt(T initialValue = T()) { store(initialValue); }

  nt(const nt &other) { store(other.load()); }

  ~nt() {}

  nt<T> &operator++() {
    ++value;
    return *this;
  }

  nt<T> operator++(int) {
    nt<T> temp = *this;
    ++value;
    return temp;
  }

  nt<T> &operator=(const T &newValue) {
    store(newValue);
    return *this;
  }

  operator T() const { return load(); }

  T load(std::memory_order __m = std::memory_order_seq_cst,
#ifdef NO_CC
    bool nt = true
#else
    bool nt = false
#endif
    ) const {
    if (nt) {
#ifdef STAT_NT_POINTER
      Statistic::atomic_load_counter++;
#endif
#ifdef SNIPER
      SimAccessCXLType2();
#else
      clflush((void *)&value, sizeof(T));
#endif
    }
    T ret = __atomic_load_n(&value, (int)__m);
#ifdef SNIPER
    SimAccessReset();
#endif
    return ret;
  }

  void store(T newValue, std::memory_order __m = std::memory_order_seq_cst,
    #ifdef NO_CC
    bool nt = true
#else
    bool nt = false
#endif
    ) {
#ifdef SNIPER
    if (nt) {
      SimAccessCXLType2();
      __atomic_store_n(&value, newValue, (int)__m);
      SimAccessReset();
    } else {
      __atomic_store_n(&value, newValue, (int)__m);
    }
#else
    __atomic_store_n(&value, newValue, (int)__m);
    if (nt) {
      // fprintf(stderr, "[%s] bypass:%d called clwb\n", __func__, nt);
      clwb((void *)&value, sizeof(T));
    }
#endif
  }

  void flush() {
#ifdef SNIPER
    SimAccessCXLType2();
    SimAccessReset();
#else
    clflush((void *)&value, sizeof(T));
#endif
  }

  T fetch_add(T i, 
#ifdef NO_CC
    bool nt = true
#else
    bool nt = false
#endif
    ) {
    T old_value;
    old_value = __atomic_fetch_add(&value, i, __ATOMIC_SEQ_CST);
    if (nt) {
      // fprintf(stderr, "[%s] bypass:%d called clwb\n", __func__, nt);
      clwb((void *)&value, sizeof(T));
    }
    return old_value;
  }

  T fetch_sub(T i, 
#ifdef NO_CC
    bool nt = true
#else
    bool nt = false
#endif
    ) {
    T old_value;
    old_value = __atomic_fetch_sub(&value, i, __ATOMIC_SEQ_CST);
    if (nt) {
      // fprintf(stderr, "[%s] bypass:%d called clwb\n", __func__, nt);
      clwb((void *)&value, sizeof(T));
    }
    return old_value;
  }

  T fetch_xor(T i, 
#ifdef NO_CC
    bool nt = true
#else
    bool nt = false
#endif
    ) {
    T old_value;
    old_value = __atomic_fetch_xor(&value, i, __ATOMIC_SEQ_CST);
    if (nt) {
      // fprintf(stderr, "[%s] bypass:%d called clwb\n", __func__, nt);
      clwb((void *)&value, sizeof(T));
    }
    return old_value;
  }

  bool
  compare_exchange_strong(T &expected, T desired,
                          std::memory_order __m = std::memory_order_seq_cst,
#ifdef NO_CC
    bool nt = true
#else
    bool nt = false
#endif
    ) {
#ifdef SNIPER
    std::cout << "Doing sniper cas" << std::endl;
    bool equal =
        __atomic_compare_exchange_n(&value, &expected, desired, 0, (int)__m,
                                    (int)std::__cmpexch_failure_order(__m));

    uint64_t expected_val = convert_to_uint64(expected);
    uint64_t desired_val = convert_to_uint64(desired);

    SimMCAS1(reinterpret_cast<uint64_t>(&value), expected_val);
    SimMCAS2(desired_val, sizeof(T));
    return equal;
#else
    bool equal =
        __atomic_compare_exchange_n(&value, &expected, desired, 0, (int)__m,
                                    (int)std::__cmpexch_failure_order(__m));
    if (nt) {
#ifdef STAT_NT_POINTER
      Statistic::atomic_cas_counter++;
#endif
      clwb((void *)&value, sizeof(T));
    }
    return equal;
#endif
  }

private:
  T value;
};
#endif

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
#ifdef NO_CC
    bool nt = true
#else
    bool nt = false
#endif
    ) const {
    if (nt) {
#ifdef SNIPER
      SimAccessCXLType2();
#else
      clflush((void *)&value, sizeof(T));
#endif
    }
    T ret = __atomic_load_n(&value, (int)__m);
#ifdef SNIPER
    SimAccessReset();
#endif
    return ret;
  }

  void store(T newValue, std::memory_order __m = std::memory_order_seq_cst,
#ifdef NO_CC
    bool nt = true
#else
    bool nt = false
#endif
    ) {
#ifdef SNIPER
    if (nt) {
      SimAccessCXLType2();
      __atomic_store_n(&value, newValue, (int)__m);
      SimAccessReset();
    } else {
      __atomic_store_n(&value, newValue, (int)__m);
    }
#else
    __atomic_store_n(&value, newValue, (int)__m);
    if (nt) {
      clwb((void *)&value, sizeof(T));
    }
#endif
  }

  T fetch_add(T i, 
#ifdef NO_CC
    bool nt = true
#else
    bool nt = false
#endif
    ) {
    T old_value;
    old_value = __atomic_add_fetch(&value, i, __ATOMIC_SEQ_CST);
#ifdef NO_CC_CAS_W_FLUSH
    if (nt) {
      // fprintf(stderr, "[%s] bypass:%d called clwb\n", __func__, nt);
      clwb((void *)&value, sizeof(T));
    }
#endif
    return old_value;
  }

  T fetch_sub(T i, 
#ifdef NO_CC
    bool nt = true
#else
    bool nt = false
#endif
    ) {
    T old_value;
    old_value = __atomic_fetch_sub(&value, i, __ATOMIC_SEQ_CST);
#ifdef NO_CC_CAS_W_FLUSH
    if (nt) {
      // fprintf(stderr, "[%s] bypass:%d called clwb\n", __func__, nt);
      clwb((void *)&value, sizeof(T));
    }
#endif
    return old_value;
  }

  T fetch_xor(T i, 
#ifdef NO_CC
    bool nt = true
#else
    bool nt = false
#endif
    ) {
    T old_value;
    old_value = __atomic_fetch_xor(&value, i, __ATOMIC_SEQ_CST);
#ifdef NO_CC_CAS_W_FLUSH
    if (nt) {
      // fprintf(stderr, "[%s] bypass:%d called clwb\n", __func__, nt);
      clwb((void *)&value, sizeof(T));
    }
#endif
    return old_value;
  }

  bool
  compare_exchange_strong(T &expected, T desired,
                          std::memory_order __m = std::memory_order_seq_cst,
#ifdef NO_CC
                          bool nt = true
#else
                          bool nt = false
#endif
    ) {
#ifdef SNIPER
    bool equal =
        __atomic_compare_exchange_n(&value, &expected, desired, 0, (int)__m,
                                    (int)std::__cmpexch_failure_order(__m));
    SimMCAS1(reinterpret_cast<uint64_t>(&value), expected);
    SimMCAS2(desired, sizeof(T));
    return equal;
#else
    bool equal =
        __atomic_compare_exchange_n(&value, &expected, desired, 0, (int)__m,
                                    (int)std::__cmpexch_failure_order(__m));
#ifdef NO_CC_CAS_W_FLUSH
    if (nt) {
      // fprintf(stderr, "[%s] bypass:%d called clwb\n", __func__, nt);
      clwb((void *)&value, sizeof(T));
    }
#endif
    return equal;
#endif
  }

private:
  alignas(CACHE_LINE_SIZE) T value;
};
#else

template <typename T> class nt {
  static_assert(sizeof(T) == sizeof(uint64_t), "T must be 64-bit type");

public:
  nt(T initialValue = T()) {
    lock = 0;
    value = (T *)uncacheable.malloc(sizeof(T));
    store(initialValue);
  }

  nt(const nt &other) {
    lock = 0;
    value = (T *)uncacheable.malloc(sizeof(T));
    store(other.load());
  }

  nt(nt &&other) noexcept {
    lock = 0;
    value = other.value;
    other.value = nullptr;
  }

  nt &operator=(const nt &other) {
    if (this != &other) {
      lock = 0;
      store(other.load());
    }
    return *this;
  }

  nt &operator=(nt &&other) noexcept {
    if (this != &other) {
      lock = 0;
      uncacheable.free(value);
      value = other.value;
      other.value = nullptr;
    }
    return *this;
  }

  nt &operator=(T newValue) {
    lock = 0;
    store(newValue);
    return *this;
  }

  ~nt() { uncacheable.free(value); }

  nt<T> &operator++() {
    *value += 1;
    return *this;
  }

  nt<T> operator++(int) {
    nt<T> temp = *this;
    *value += 1;
    return temp;
  }

  operator T() const { return load(); }

  T load(std::memory_order __m = std::memory_order_seq_cst,
#ifdef NO_CC
         bool nt = true
#else
         bool nt = false
#endif
    ) const {
    return *value;
  }

  void store(T newValue, std::memory_order __m = std::memory_order_seq_cst,
#ifdef NO_CC
             bool nt = true
#else
             bool nt = false
#endif
    ) {
    uint8_t expected = 0;
    while (!__atomic_compare_exchange_n(&lock, &expected, 1, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      ;
    *value = newValue;
    lock = 0;
  }

  T fetch_add(T i, 
#ifdef NO_CC
    bool nt = true
#else
    bool nt = false
#endif
    ) {
    uint8_t expected = 0;
    while (!__atomic_compare_exchange_n(&lock, &expected, 1, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      ;
    T old_value = *value;
    *value += i;
    lock = 0;
    return old_value;
  }

  T fetch_sub(T i, 
#ifdef NO_CC
    bool nt = true
#else
    bool nt = false
#endif
    ) {
    uint8_t expected = 0;
    while (!__atomic_compare_exchange_n(&lock, &expected, 1, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      ;
    T old_value = *value;
    *value -= i;
    lock = 0;
    return old_value;
  }

  bool
  compare_exchange_strong(T &expected, T desired,
                          std::memory_order __m = std::memory_order_seq_cst,
#ifdef NO_CC
                          bool nt = true
#else
                          bool nt = false
#endif
    ) {
    uint8_t lock_expected = 0;
    if (__atomic_compare_exchange_n(&lock, &lock_expected, 1, 0, (int)__m,
                                    (int)std::__cmpexch_failure_order(__m))) {
      if (*value == expected) {
        *value = desired;
        lock = 0;
        return true;
      }
      lock = 0;
    }
    return false;
  }

private:
  uint8_t lock;
  T *value;
};
#endif
#endif