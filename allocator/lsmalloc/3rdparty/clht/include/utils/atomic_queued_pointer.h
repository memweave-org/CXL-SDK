#pragma once
#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

#include "utils/bypass_cache.h"
#include "utils/sim_id.h"
#include "atomic_queued_variable.h"

using namespace std;

template <typename T>
class nt_pointer;

template <typename T>
struct is_nt_pointer : std::false_type {};

template <typename T>
struct is_nt_pointer<nt_pointer<T>> : std::true_type {};

template <typename T>
class nt_pointer {
 public:
  nt_pointer(T *init = nullptr) { store(init); }
  nt_pointer(const nt_pointer &other) { store(other.load()); }

  template <typename... Args>
  void allocate(Args &&...args) {
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
      AtomicRequestQueue::pend_load_req(&ptr);     
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
      AtomicRequestQueue::pend_store_req(&ptr);
    }
  }

  void flush() { clwb((void *)&ptr, sizeof(T *)); }

  bool compare_exchange_strong(
      T *expected, T *desired,
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
            AtomicRequestQueue::pend_cas_req(&ptr);
    }
    return equal;
  }

  void free() {
    delete load();
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

template <typename T>
class nt_pointer<T[]> {
 public:
  nt_pointer(T *init = nullptr) {
    size = 0;
    store(init);
  }
  nt_pointer(const nt_pointer &other) {
    size = other.size;
    store(other.load());
  }

  template <typename... Args> void allocate(size_t size, Args &&...args) {
    this->size = size;
    ptr = (T*)malloc(sizeof(T) * size);
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

  void flush_elements(size_t size) { clwb((void *)&ptr, sizeof(T) * size); }

  T *load(std::memory_order __m = std::memory_order_seq_cst,
#if defined(NO_CC) || defined(USE_NO_CC_QUEUE)
          bool nt = true
#else
          bool nt = false
#endif
          ) const {
    if (nt) {
      AtomicRequestQueue::pend_load_req(&ptr);

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
      AtomicRequestQueue::pend_store_req(&ptr);

    }
  }

  bool compare_exchange_strong(
      T *expected, T *desired,
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
      AtomicRequestQueue::pend_cas_req(&ptr);
      // clwb((void *)ptr, sizeof(T));
    }
    return equal;
  }

  void free() {
    for (size_t i = 0; i < size; i++) {
      ptr[i].~T(); // 显式调用析构函数
    }
    if (size != 0) {
      ::free(ptr); // 释放内存
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
    size = rhs.size;
    return *this;
  }

  bool operator==(const nt_pointer &rhs) {
    return load(memory_order_seq_cst, false) ==
           rhs.load(memory_order_seq_cst, false);
  }
  bool operator==(const T *rhs) {
    return load(memory_order_seq_cst, false) == rhs;
  }

 private:
   T *ptr;
   size_t size;
};