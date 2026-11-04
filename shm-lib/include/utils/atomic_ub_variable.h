#pragma once
#include "utils/ub_atomic.h"

// Keep the value inline, so a placement-constructed wrapper operates on exactly
// the caller's mapped address. The legacy bool argument is API compatibility
// only: neither true nor false switches a UB atomic to a simulation backend.
template<class T, size_t Alignment = alignof(T)> class ub_nt {
public:
  ub_nt(T initial = T()) { store(initial); }
  ub_nt(const ub_nt& other) { store(other.load()); }
  ub_nt& operator=(const ub_nt& other) { store(other.load()); return *this; }
  ub_nt& operator=(T value) { store(value); return *this; }
  ub_nt& operator++() { fetch_add(1); return *this; }
  ub_nt operator++(int) { return ub_nt(fetch_add(1)); }

  operator T() const { return load(); }

  // Arithmetic operators
  T operator+(const T &other) const { return load() + other; }
  T operator-(const T &other) const { return load() - other; }
  T operator*(const T &other) const { return load() * other; }
  T operator/(const T &other) const { return load() / other; }
  T operator%(const T &other) const { return load() % other; }

  // Bitwise operators
  T operator&(const T &other) const { return load() & other; }
  T operator|(const T &other) const { return load() | other; }
  T operator^(const T &other) const { return load() ^ other; }
  T operator<<(int shift) const { return load() << shift; }
  T operator>>(int shift) const { return load() >> shift; }
  T operator~() const { return ~load(); }

  // Comparison operators
  bool operator==(const T &other) const { return load() == other; }
  bool operator!=(const T &other) const { return load() != other; }
  bool operator<(const T &other) const { return load() < other; }
  bool operator<=(const T &other) const { return load() <= other; }
  bool operator>(const T &other) const { return load() > other; }
  bool operator>=(const T &other) const { return load() >= other; }

  // Unary operators
  T operator+() const { return +load(); }
  T operator-() const { return -load(); }
  bool operator!() const { return !load(); }

  T load(std::memory_order order = std::memory_order_seq_cst, bool = true) const {
    return ub_atomic::load(&value, order);
  }
  void store(T desired, std::memory_order order = std::memory_order_seq_cst, bool = true) {
    ub_atomic::store(&value, desired, order);
  }
  T fetch_add(T amount, bool = true) { return ub_atomic::fetch_add(&value, amount); }
  T fetch_sub(T amount, bool = true) { return ub_atomic::fetch_sub(&value, amount); }
  T fetch_xor(T bits, bool = true) { return ub_atomic::fetch_xor(&value, bits); }
  bool compare_exchange_strong(T& expected, T desired,
                               std::memory_order order = std::memory_order_seq_cst,
                               bool = true) {
    return ub_atomic::compare_exchange(&value, expected, desired, order);
  }
  void flush() { ub_atomic::flush(&value); }
  T* get_raw_ptr() { return &value; }
  const T* get_raw_ptr() const { return &value; }

private:
  alignas(Alignment) T value;
};
template<class T> using nt = ub_nt<T>;
template<class T> using aligned_nt = ub_nt<T, CACHE_LINE_SIZE>;
