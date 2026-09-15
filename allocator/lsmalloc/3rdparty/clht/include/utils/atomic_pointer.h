#pragma once
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <cstring>
#include <vector>
#include "shm/mm.h"

#include "utils/bypass_cache.h"
#include "utils/sim_id.h"

using namespace std;

#ifdef QUEUE_SIM_CAS
#include "atomic_queued_pointer.h"
#else

template <typename T> class nt_pointer;

template <typename T> struct is_nt_pointer : std::false_type {};

template <typename T> struct is_nt_pointer<nt_pointer<T>> : std::true_type {};

#ifdef NT_SIM
template <typename T> class nt_pointer {
public:
  // ~nt_pointer() { free(); }

  nt_pointer(T *init = nullptr) {
    // cache_value = new T *[SimThreadInfo::worker_machine_count]();
    // cache_available = new char[SimThreadInfo::worker_machine_count]();
    cache_value.resize(SimThreadInfo::worker_machine_count);
    cache_available.resize(SimThreadInfo::worker_machine_count);
    store(init);
  }
  nt_pointer(std::nullptr_t) {
    cache_value.resize(SimThreadInfo::worker_machine_count);
    cache_available.resize(SimThreadInfo::worker_machine_count);
    store(nullptr);
  }
  nt_pointer(const nt_pointer &other) {
    // cache_value = new T *[SimThreadInfo::worker_machine_count]();
    // cache_available = new char[SimThreadInfo::worker_machine_count]();
    cache_value.resize(SimThreadInfo::worker_machine_count);
    cache_available.resize(SimThreadInfo::worker_machine_count);
    T *tmp = other.load();
    store(tmp);
  }

  template <typename... Args> void allocate(Args &&...args) {
    store(new T(std::forward<Args>(args)...));
  }

  T *load(memory_order __m = memory_order_seq_cst, bool nt = true) const {
    if (!cache_available[SimThreadInfo::worker_machine_id]) [[unlikely]] {
      nt = true;
    }
    if (nt) {
      const_cast<T *&>(cache_value[SimThreadInfo::worker_machine_id]) =
          __atomic_load_n(&ptr, (int)__m);
      const_cast<char &>(cache_available[SimThreadInfo::worker_machine_id]) =
          true;
    }
    return __atomic_load_n(&cache_value[SimThreadInfo::worker_machine_id],
                           (int)__m);
  }

  void store(T *new_value, memory_order __m = memory_order_seq_cst,
             bool nt = true) {
    T **addr;
    if (nt) {
      addr = &ptr;
      // __atomic_store_n(&(cache_value[SimThreadInfo::worker_machine_id]),
      //                  new_value, (int)__m);
      cache_value[SimThreadInfo::worker_machine_id] = new_value;
      const_cast<char &>(cache_available[SimThreadInfo::worker_machine_id]) =
          true;
    } else {
      addr = &cache_value[SimThreadInfo::worker_machine_id];
    }
    // __atomic_store_n(addr, new_value, (int)__m);
    *addr = new_value;
  }

  bool
  compare_exchange_strong(T *expected, T *desired,
                          std::memory_order __m = std::memory_order_seq_cst,
                          bool nt = true) {
    if (!cache_available[SimThreadInfo::worker_machine_id]) [[unlikely]] {
      nt = true;
    }
    if (nt) {
      bool equal =
          __atomic_compare_exchange_n(&ptr, &expected, desired, 0, (int)__m,
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

  void free() {
    // delete load();
    // store(nullptr);
  }

  T &operator*() { return *load(memory_order_seq_cst, false); }
  T *operator->() { return load(memory_order_seq_cst, false); }
  operator T *() { return load(memory_order_seq_cst, false); }
  // operator  const T * () { return load(false); }

  nt_pointer &operator=(T *rhs) {
    store(rhs);
    return *this;
  }

  nt_pointer &operator=(const nt_pointer &rhs) {
    store(rhs.load(memory_order_seq_cst, false));
    return *this;
  }

  bool operator==(const nt_pointer &rhs) {
    return load(memory_order_seq_cst, false) ==
           rhs.load(memory_order_seq_cst, false);
  }

  bool operator==(const T *rhs) const {
    return load(memory_order_seq_cst, false) == rhs;
  }
  bool operator!=(const T *rhs) const {
    return load(memory_order_seq_cst, false) != rhs;
  }
  bool operator==(T *rhs) const {
    return load(memory_order_seq_cst, false) == rhs;
  }
  bool operator!=(T *rhs) const {
    return load(memory_order_seq_cst, false) != rhs;
  }
  bool operator==(std::nullptr_t rhs) const {
    return load(memory_order_seq_cst, false) == nullptr;
  }
  bool operator!=(std::nullptr_t rhs) const {
    return load(memory_order_seq_cst, false) != nullptr;
  }

private:
  T *ptr;
  // T **cache_value;
  // char *cache_available;
  mutable std::vector<T *> cache_value;
  mutable std::vector<char> cache_available;
};

template <typename T> class nt_pointer<T[]> {
public:
  // ~nt_pointer() { free(); }

  nt_pointer(T *init = nullptr) {
    cache_value.resize(SimThreadInfo::worker_machine_count);
    cache_available.resize(SimThreadInfo::worker_machine_count);
    store(init);
  }
  nt_pointer(std::nullptr_t) {
    cache_value.resize(SimThreadInfo::worker_machine_count);
    cache_available.resize(SimThreadInfo::worker_machine_count);
    store(nullptr);
  }
  nt_pointer(const nt_pointer &other) {
    cache_value.resize(SimThreadInfo::worker_machine_count);
    cache_available.resize(SimThreadInfo::worker_machine_count);
    store(other.load());
  }

  template <typename... Args> void allocate(size_t size, Args &&...args) {
    store(new T[size]());
    for (size_t i = 0; i < size; i++) {
      if constexpr (is_nt_pointer<T>::value) {
        ptr[i].allocate(std::forward<Args>(args)...);
      } else {
        ptr[i] = T(std::forward<Args>(args)...);
      }
    }
  }

  T *load(memory_order __m = memory_order_seq_cst, bool nt = true) const {
    if (!cache_available[SimThreadInfo::worker_machine_id]) [[unlikely]] {
      nt = true;
    }
    if (nt) {
      const_cast<T *&>(cache_value[SimThreadInfo::worker_machine_id]) =
          __atomic_load_n(&ptr, (int)__m);
      const_cast<char &>(cache_available[SimThreadInfo::worker_machine_id]) =
          true;
    }
    return __atomic_load_n(&cache_value[SimThreadInfo::worker_machine_id],
                           (int)__m);
  }

  void store(T *newValue, memory_order __m = memory_order_seq_cst,
             bool nt = true) {
    T **addr;
    if (nt) {
      addr = &ptr;
      __atomic_store_n(&cache_value[SimThreadInfo::worker_machine_id], newValue,
                       (int)__m);
      cache_available[SimThreadInfo::worker_machine_id] = true;
    } else {
      addr = &cache_value[SimThreadInfo::worker_machine_id];
    }
    __atomic_store_n(addr, newValue, (int)__m);
  }

  bool
  compare_exchange_strong(T *expected, T *desired,
                          std::memory_order __m = std::memory_order_seq_cst,
                          bool nt = true) {
    bool equal =
        __atomic_compare_exchange_n(&ptr, &expected, desired, 0, (int)__m,
                                    (int)std::__cmpexch_failure_order(__m));
    if (equal && nt) {
      clwb((void *)&ptr, sizeof(T *));
      // clwb((void *)ptr, sizeof(T));
    }
    return equal;
  }

  void free() {
    delete[] load();
    store(nullptr);
  }

  T &operator*() { return *load(memory_order_seq_cst, false); }
  T *operator->() { return load(memory_order_seq_cst, false); }
  operator T *() { return load(memory_order_seq_cst, false); }
  T &operator[](uint64_t idx) { return load(memory_order_seq_cst, false)[idx]; }
  T &operator[](int64_t idx) { return load(memory_order_seq_cst, false)[idx]; }

  nt_pointer &operator=(T *rhs) {
    store(rhs);
    return *this;
  }

  nt_pointer &operator=(const nt_pointer &rhs) {
    store(rhs.load(memory_order_seq_cst, false));
    return *this;
  }

  bool operator==(const nt_pointer &rhs) {
    return load(memory_order_seq_cst, false) ==
           rhs.load(memory_order_seq_cst, memory_order_seq_cst, false);
  }
  bool operator==(const T *rhs) {
    return load(memory_order_seq_cst, false) == rhs;
  }

private:
  T *ptr;
  mutable std::vector<T *> cache_value;
  mutable std::vector<char> cache_available;
};
#else
template <typename T> class nt_pointer {
public:
  // ~nt_pointer() { free(); }

  nt_pointer(T *init = nullptr) { store(init); }
  nt_pointer(const nt_pointer &other) { store(other.load()); }

  template <typename... Args> void allocate(Args &&...args) {
    store(new T(std::forward<Args>(args)...));
  }

  T *load(std::memory_order __m = std::memory_order_seq_cst,
#if defined(NO_CC) || defined(USE_NO_CC_QUEUE)
          bool nt = true
#else
          bool nt = false
#endif
  ) const {
    if (nt) {
      clflush((void *)&ptr, sizeof(T *));
    }
    return ptr;
  }

  void store(T *value, std::memory_order __m = std::memory_order_seq_cst,
#if defined(NO_CC) || defined(USE_NO_CC_QUEUE)
             bool nt = true
#else
             bool nt = false
#endif
  ) {
    ptr = value;
    if (nt) {
      clwb((void *)&ptr, sizeof(T *));
    }
  }

  void flush() { clflush((void *)&ptr, sizeof(T *)); }

  bool
  compare_exchange_strong(T *expected, T *desired,
                          std::memory_order __m = std::memory_order_seq_cst,
#if defined(NO_CC) || defined(USE_NO_CC_QUEUE)
                          bool nt = true
#else
                          bool nt = false
#endif
  ) {
    bool equal =
        __atomic_compare_exchange_n(&ptr, &expected, desired, 0, (int)__m,
                                    (int)std::__cmpexch_failure_order(__m));
    if (nt) {
      clwb((void *)&ptr, sizeof(T *));
    }
    return equal;
  }

  void free() {
    // delete load();
    store(nullptr);
  }

  T &operator*() { return *load(memory_order_seq_cst, false); }
  T *operator->() { return load(memory_order_seq_cst, false); }
  operator T *() { return load(memory_order_seq_cst, false); }
  // operator  const T * () { return load(false); }

  nt_pointer &operator=(T *rhs) {
    store(rhs);
    return *this;
  }

  nt_pointer &operator=(const nt_pointer &rhs) {
    store(rhs.load(memory_order_seq_cst, false));
    return *this;
  }

  bool operator==(const nt_pointer &rhs) {
    return load(memory_order_seq_cst, false) ==
           rhs.load(memory_order_seq_cst, false);
  }

  bool operator==(const T *rhs) const {
    return load(memory_order_seq_cst, false) == rhs;
  }
  bool operator!=(const T *rhs) const {
    return load(memory_order_seq_cst, false) != rhs;
  }
  bool operator==(T *rhs) const {
    return load(memory_order_seq_cst, false) == rhs;
  }
  bool operator!=(T *rhs) const {
    return load(memory_order_seq_cst, false) != rhs;
  }
  bool operator==(std::nullptr_t rhs) const {
    return load(memory_order_seq_cst, false) == nullptr;
  }
  bool operator!=(std::nullptr_t rhs) const {
    return load(memory_order_seq_cst, false) != nullptr;
  }

private:
  T *ptr;
};

template <typename T> class nt_pointer<T[]> {
public:
  // ~nt_pointer() { free(); }

  nt_pointer(T *init = nullptr) { store(init); }
  nt_pointer(const nt_pointer &other) { store(other.load()); }

  template <typename... Args> void allocate(size_t size, Args &&...args) {
    this->size = size;
    ptr = (T *)cacheable.malloc(sizeof(T) * size);
    for (size_t i = 0; i < size; i++) {
      if constexpr (is_nt_pointer<T>::value) {
        new (&ptr[i]) T();
        ptr[i].allocate(std::forward<Args>(args)...);
      } else {
        // ptr[i] = T(std::forward<Args>(args)...);
        new (&ptr[i]) T(std::forward<Args>(args)...);
      }
    }
  }

  void flush_elements(size_t size) { clflush((void *)&ptr, sizeof(T) * size); }

  T *load(std::memory_order __m = std::memory_order_seq_cst,
#if defined(NO_CC) || defined(USE_NO_CC_QUEUE)
          bool nt = true
#else
          bool nt = false
#endif
  ) const {
    if (nt) {
      clflush((void *)&ptr, sizeof(T *));
    }
    return ptr;
  }

  void store(T *newValue,
#if defined(NO_CC) || defined(USE_NO_CC_QUEUE)
             bool nt = true
#else
             bool nt = false
#endif
  ) {
    ptr = newValue;
    if (nt) {
      clwb((void *)&ptr, sizeof(T *));
    }
  }

  bool
  compare_exchange_strong(T *expected, T *desired,
                          std::memory_order __m = std::memory_order_seq_cst,
#if defined(NO_CC) || defined(USE_NO_CC_QUEUE)
                          bool nt = true
#else
                          bool nt = false
#endif
  ) {
    bool equal =
        __atomic_compare_exchange_n(&ptr, &expected, desired, 0, (int)__m,
                                    (int)std::__cmpexch_failure_order(__m));
    if (equal && nt) {
      clwb((void *)&ptr, sizeof(T *));
    }
    return equal;
  }

  void free() {
    T* p = load();
    if (!p) return;
    if (size != 0) {
      if constexpr (!std::is_trivially_destructible_v<T>) {
        for (size_t i = 0; i < size; ++i) {
          p[i].~T();
        }
      }
      cacheable.free(p);  // 与 cacheable.malloc 匹配
    }
    store(nullptr);
    size = 0;
  }

  T &operator*() { return *load(memory_order_seq_cst, false); }
  T *operator->() { return load(memory_order_seq_cst, false); }
  operator T *() { return load(memory_order_seq_cst, false); }
  T &operator[](uint64_t idx) { return load(memory_order_seq_cst, false)[idx]; }
  T &operator[](int64_t idx) { return load(memory_order_seq_cst, false)[idx]; }

  nt_pointer &operator=(T *rhs) {
    store(rhs);
    return *this;
  }

  nt_pointer &operator=(const nt_pointer &rhs) {
    store(rhs.load(memory_order_seq_cst, false));
    return *this;
  }
  
  bool operator==(const nt_pointer &rhs) {
    return load(memory_order_seq_cst, false) ==
           rhs.load(memory_order_seq_cst, memory_order_seq_cst, false);
  }
  bool operator==(const T *rhs) {
    return load(memory_order_seq_cst, false) == rhs;
  }

private:
  T *ptr;
  size_t size;
};
#endif
#endif