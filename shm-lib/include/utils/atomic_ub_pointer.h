#pragma once
#include <atomic>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <utility>
#include "shm/mm.h"
#include "utils/ub_atomic.h"

template<class T> class nt_pointer;
template<class T> struct is_nt_pointer : std::false_type {};
template<class T> struct is_nt_pointer<nt_pointer<T>> : std::true_type {};

template<class T> class nt_pointer {
public:
  nt_pointer(T* initial = nullptr) { store(initial); }
  nt_pointer(const nt_pointer& other) { store(other.load()); }
  template<class... Args> void allocate(Args&&... args) {
    store(new T(std::forward<Args>(args)...));
  }
  T* load(std::memory_order order = std::memory_order_seq_cst, bool = true) const {
    return ub_atomic::load(&ptr, order);
  }
  void store(T* desired, std::memory_order order = std::memory_order_seq_cst, bool = true) {
    ub_atomic::store(&ptr, desired, order);
  }
  // Preserve nt_pointer's existing by-value expected contract.
  bool compare_exchange_strong(T* expected, T* desired,
                               std::memory_order order = std::memory_order_seq_cst,
                               bool = true) {
    return ub_atomic::compare_exchange(&ptr, expected, desired, order);
  }
  void flush() { ub_atomic::flush(&ptr); }
  void free() { delete load(); store(nullptr); }

  T &operator*() { return *load(); }
  T *operator->() { return load(); }
  operator T *() const { return load(); }
  operator volatile T *() const { return const_cast<volatile T*>(load()); }
  operator T *() const volatile { return const_cast<nt_pointer*>(this)->load(); }
  operator volatile T *() const volatile { return const_cast<volatile T*>(const_cast<nt_pointer*>(this)->load()); }

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
  T* ptr;
};

// Array metadata remains process-independent: the mapped pointer and element
// count only. Shared elements use the existing NC allocator in split mode.
template<class T> class nt_pointer<T[]> : public nt_pointer<T> {
  using Base = nt_pointer<T>;
public:
  nt_pointer(T* initial = nullptr) : Base(initial) {}
  nt_pointer(const nt_pointer& other) : Base(other), size(other.size) {}
  nt_pointer& operator=(const nt_pointer& other) {
    Base::operator=(other); size = other.size; return *this;
  }
  nt_pointer& operator=(T* desired) { Base::store(desired); return *this; }
  void store(T* desired, bool = true) { Base::store(desired); }
  template<class... Args> void allocate(size_t count, Args&&... args) {
    if (count > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_alloc();
#ifdef USE_CXL
    auto* elements = static_cast<T*>(shared_atomic_memory().malloc(sizeof(T) * count));
#else
    auto* elements = static_cast<T*>(::malloc(sizeof(T) * count));
#endif
    if (!elements) throw std::bad_alloc();
    for (size_t i = 0; i < count; ++i) {
      if constexpr (is_nt_pointer<T>::value) {
        new (&elements[i]) T();
        elements[i].allocate(std::forward<Args>(args)...);
      } else {
        new (&elements[i]) T(std::forward<Args>(args)...);
      }
    }
    size = count;
    Base::store(elements);
  }
  void flush_elements(size_t count) { clwb(Base::load(), sizeof(T) * count); }
  void free() {
    T* elements = Base::load();
    for (size_t i = 0; i < size; ++i) elements[i].~T();
    if (size) {
#ifdef USE_CXL
      shared_atomic_memory().free(elements);
#else
      ::free(elements);
#endif
    }
    Base::store(nullptr); size = 0;
  }
  T& operator[](uint64_t index) { return Base::load()[index]; }
  T& operator[](int64_t index) { return Base::load()[index]; }
private:
  size_t size = 0;
};
