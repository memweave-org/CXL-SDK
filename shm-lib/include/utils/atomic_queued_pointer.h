#pragma once
#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>
#include "shm/mm.h"

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
#ifdef SHM_LIB_WITH_UBSM
    return __atomic_load_n(&ptr, (int)__m);
#else
    return ptr;
#endif
  }

  void store(T *value, std::memory_order __m = std::memory_order_seq_cst,
#if defined(NO_CC) || defined(USE_NO_CC_QUEUE)
             bool nt = true
#else
             bool nt = false
#endif
             ) {
#ifdef SHM_LIB_WITH_UBSM
    __atomic_store_n(&ptr, value, (int)__m);
#else
    ptr = value;
#endif
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

  T &operator*() { return *load(); }
  T *operator->() { return load(); }
  operator T *() const { return load(); }
  operator volatile T *() const { return const_cast<volatile T*>(load()); }
  operator T *() const volatile { return const_cast<nt_pointer*>(this)->load(); }
  operator volatile T *() const volatile { return const_cast<volatile T*>(const_cast<nt_pointer*>(this)->load()); }
  // operator  const T * () { return load(); }

  nt_pointer &operator=(T *rhs) {
    store(rhs);
    return *this;
  }

  nt_pointer &operator=(const nt_pointer &rhs) {
    store(rhs.load());
    return *this;
  }

  bool operator==(const nt_pointer &rhs) const {
    return load() == rhs.load();
  }
  bool operator==(const nt_pointer &rhs) const volatile {
    return const_cast<nt_pointer*>(this)->load() == const_cast<nt_pointer*>(&rhs)->load();
  }

  bool operator==(const T *rhs) const {
    return load() == rhs;
  }
  bool operator==(const T *rhs) const volatile {
    return const_cast<nt_pointer*>(this)->load() == rhs;
  }
  bool operator!=(const T *rhs) const {
    return load() != rhs;
  }
  bool operator!=(const T *rhs) const volatile {
    return const_cast<nt_pointer*>(this)->load() != rhs;
  }
  bool operator==(T *rhs) const {
    return load() == rhs;
  }
  bool operator==(T *rhs) const volatile {
    return const_cast<nt_pointer*>(this)->load() == rhs;
  }
  bool operator!=(T *rhs) const {
    return load() != rhs;
  }
  bool operator!=(T *rhs) const volatile {
    return const_cast<nt_pointer*>(this)->load() != rhs;
  }
  bool operator==(std::nullptr_t rhs) const {
    return load() == nullptr;
  }
  bool operator==(std::nullptr_t rhs) const volatile {
    return const_cast<nt_pointer*>(this)->load() == nullptr;
  }
  bool operator!=(std::nullptr_t rhs) const {
    return load() != nullptr;
  }
  bool operator!=(std::nullptr_t rhs) const volatile {
    return const_cast<nt_pointer*>(this)->load() != nullptr;
  }
  // Support for NULL (which may be 0 or 0L)
  bool operator==(int rhs) const {
    return rhs == 0 && load() == nullptr;
  }
  bool operator==(int rhs) const volatile {
    return rhs == 0 && const_cast<nt_pointer*>(this)->load() == nullptr;
  }
  bool operator!=(int rhs) const {
    return rhs != 0 || load() != nullptr;
  }
  bool operator!=(int rhs) const volatile {
    return rhs != 0 || const_cast<nt_pointer*>(this)->load() != nullptr;
  }
  bool operator==(long rhs) const {
    return rhs == 0 && load() == nullptr;
  }
  bool operator==(long rhs) const volatile {
    return rhs == 0 && const_cast<nt_pointer*>(this)->load() == nullptr;
  }
  bool operator!=(long rhs) const {
    return rhs != 0 || load() != nullptr;
  }
  bool operator!=(long rhs) const volatile {
    return rhs != 0 || const_cast<nt_pointer*>(this)->load() != nullptr;
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
    #ifdef USE_CXL
    ptr = static_cast<T*>(cacheable.malloc(sizeof(T) * size));
#else
    ptr = static_cast<T*>(malloc(sizeof(T) * size));
#endif
    if (!ptr) throw std::bad_alloc();
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

  void flush_elements(size_t size) { clwb((void *)ptr, sizeof(T) * size); }

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
#ifdef SHM_LIB_WITH_UBSM
    return __atomic_load_n(&ptr, (int)__m);
#else
    return ptr;
#endif
  }

  void store(T *newValue,
#if defined(NO_CC) || defined(USE_NO_CC_QUEUE)
             bool nt = true
#else
             bool nt = false
#endif
             ) {
#ifdef SHM_LIB_WITH_UBSM
    __atomic_store_n(&ptr, newValue, __ATOMIC_SEQ_CST);
#else
    ptr = newValue;
#endif
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
#ifdef USE_CXL
      cacheable.free(ptr);
#else
      ::free(ptr);
#endif
    }
    store(nullptr);
    size = 0;
  }

  T &operator*() { return *load(); }
  T *operator->() { return load(); }
  operator T *() const { return load(); }
  operator volatile T *() const { return const_cast<volatile T*>(load()); }
  operator T *() const volatile { return const_cast<nt_pointer*>(this)->load(); }
  operator volatile T *() const volatile { return const_cast<volatile T*>(const_cast<nt_pointer*>(this)->load()); }
  T &operator[](uint64_t idx) { return load()[idx]; }
  T &operator[](int64_t idx) { return load()[idx]; }

  nt_pointer &operator=(T *rhs) {
    store(rhs);
    return *this;
  }

  nt_pointer &operator=(const nt_pointer &rhs) {
    store(rhs.load());
    size = rhs.size;
    return *this;
  }

  bool operator==(const nt_pointer &rhs) const {
    return load() == rhs.load();
  }
  bool operator==(const nt_pointer &rhs) const volatile {
    return const_cast<nt_pointer*>(this)->load() == const_cast<nt_pointer*>(&rhs)->load();
  }
  bool operator==(const T *rhs) const {
    return load() == rhs;
  }
  bool operator==(const T *rhs) const volatile {
    return const_cast<nt_pointer*>(this)->load() == rhs;
  }
  bool operator!=(const T *rhs) const {
    return load() != rhs;
  }
  bool operator!=(const T *rhs) const volatile {
    return const_cast<nt_pointer*>(this)->load() != rhs;
  }
  bool operator==(std::nullptr_t rhs) const {
    return load() == nullptr;
  }
  bool operator==(std::nullptr_t rhs) const volatile {
    return const_cast<nt_pointer*>(this)->load() == nullptr;
  }
  bool operator!=(std::nullptr_t rhs) const {
    return load() != nullptr;
  }
  bool operator!=(std::nullptr_t rhs) const volatile {
    return const_cast<nt_pointer*>(this)->load() != nullptr;
  }
  // Support for NULL (which may be 0 or 0L)
  bool operator==(int rhs) const {
    return rhs == 0 && load() == nullptr;
  }
  bool operator==(int rhs) const volatile {
    return rhs == 0 && const_cast<nt_pointer*>(this)->load() == nullptr;
  }
  bool operator!=(int rhs) const {
    return rhs != 0 || load() != nullptr;
  }
  bool operator!=(int rhs) const volatile {
    return rhs != 0 || const_cast<nt_pointer*>(this)->load() != nullptr;
  }
  bool operator==(long rhs) const {
    return rhs == 0 && load() == nullptr;
  }
  bool operator==(long rhs) const volatile {
    return rhs == 0 && const_cast<nt_pointer*>(this)->load() == nullptr;
  }
  bool operator!=(long rhs) const {
    return rhs != 0 || load() != nullptr;
  }
  bool operator!=(long rhs) const volatile {
    return rhs != 0 || const_cast<nt_pointer*>(this)->load() != nullptr;
  }

 private:
   T *ptr;
   size_t size;
};