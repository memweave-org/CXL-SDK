#ifndef HELP_UPDATE_H
#define HELP_UPDATE_H

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <thread>
#include <cassert>
#include "utils/atomic_pointer.h"
#include "utils/atomic_variable.h"

// #define cas(old_val, new_val)  (old_val, new_val)

// Helper class: Encapsulates pointer value and lock bit operations
template <typename T>
class LockedPointer {
private:
  T* ptr_;

  static inline uintptr_t clear_lock_bit(uintptr_t val) {
    return val & ~static_cast<uintptr_t>(1);
  }

  static inline uintptr_t set_lock_bit(uintptr_t val) {
    return val | static_cast<uintptr_t>(1);
  }

public:
  LockedPointer(T* ptr = nullptr) : ptr_(ptr) {}

  // Create from a pointer with lock bit set
  static LockedPointer from_locked(T* locked_ptr) {
    uintptr_t val = reinterpret_cast<uintptr_t>(locked_ptr);
    return LockedPointer(reinterpret_cast<T*>(clear_lock_bit(val)));
  }

  // Get pointer with lock bit cleared
  T* get() const { return ptr_; }
  operator T*() const { return ptr_; }

  // Get pointer with lock bit set
  T* with_lock() const {
    uintptr_t val = reinterpret_cast<uintptr_t>(ptr_);
    return reinterpret_cast<T*>(set_lock_bit(val));
  }

  // Check if pointer has lock bit set
  static bool has_lock(T* ptr) {
    if (ptr == nullptr) return false;
    uintptr_t val = reinterpret_cast<uintptr_t>(ptr);
    return (val & static_cast<uintptr_t>(1)) != 0;
  }

  // Clear lock bit from pointer
  static T* clear_lock(T* ptr) {
    if (ptr == nullptr) return nullptr;
    uintptr_t val = reinterpret_cast<uintptr_t>(ptr);
    return reinterpret_cast<T*>(clear_lock_bit(val));
  }

  // Comparison operations
  bool operator==(const LockedPointer& other) const {
    return ptr_ == other.ptr_;
  }

  bool operator==(T* other) const {
    return ptr_ == other;
  }
};

template <typename T>
class HelpUpdate {
private:
  // Pointer to global pointer (using nt type)
  nt_pointer<T>* global_ptr_;
  // Pointer to replica pointer array (using nt type)
  nt_pointer<T>* replica_ptrs_;
  // Number of replicas
  size_t replica_num_;

  static bool current_cas(nt_pointer<T> &ptr, T* old_val, T* new_val) {
    T* expected = old_val;
    if (ptr.compare_exchange_strong(expected, new_val)) {
      return true;
    }
    return ptr.load(std::memory_order_seq_cst) == new_val;
  }

public:
  // The replica buffer must remain allocated and must not resize while this
  // helper is in use. Store its data pointer, not a process-private vector.
  template<class Allocator>
  HelpUpdate(nt_pointer<T> &global_ptr,
             std::vector<nt_pointer<T>, Allocator> &replica_ptrs,
             size_t replica_num)
      : global_ptr_(&global_ptr), replica_ptrs_(replica_ptrs.data()),
        replica_num_(replica_num) {}

  // Update all replicas for one round
  // @param expected_val: Expected value
  // @param start_check_id: Thread ID to start checking from
  // @return: Returns true if update succeeded and expected_val hasn't changed,
  //          false if expected_val has changed. Any return indicates successful update.
  bool help_update(T* expected_val, uint64_t start_check_id) {
    // printf("help_update expected: %p, start_check_id: %lu\n", expected_val, start_check_id);
    assert(start_check_id < replica_num_);
    LockedPointer<T> expected(expected_val);
    for (size_t i = start_check_id; i < replica_num_; ++i) {
      T* cur_global_ptr = global_ptr_->load();
      LockedPointer<T> cur_global = LockedPointer<T>::from_locked(cur_global_ptr);
      if (cur_global == expected && !LockedPointer<T>::has_lock(cur_global_ptr)) {
        return true;
      }

      T* local_r = replica_ptrs_[i].load();
      LockedPointer<T> local = LockedPointer<T>::from_locked(local_r);
      if (local != expected) {
        cur_global_ptr = global_ptr_->load();
        cur_global = LockedPointer<T>::from_locked(cur_global_ptr);
        if (cur_global != expected) {
          return false;
        }
        if (!current_cas(replica_ptrs_[i], local_r, expected.with_lock())) {
          return false;
        }
      }
    }

    // Clear the global pointer's lock bit after all replicas are updated.
    if (global_ptr_->compare_exchange_strong(expected.with_lock(), expected)) {
      return true;
    }
    return true;
  }

  // Update global pointer
  // Algorithm: Write G as A.1, then write each R[i] as A.1 in sequence,
  //            after all writes complete, write G as A.0
  bool cas_ptr(T* old_val, T* new_val) {
    T* cur_global = global_ptr_->load();

    if (LockedPointer<T>::has_lock(cur_global)) {
      help_update(LockedPointer<T>::clear_lock(cur_global), 0);
    }

    T* expected = old_val;
    LockedPointer<T> new_ptr(new_val);
    if (!global_ptr_->compare_exchange_strong(expected, new_ptr.with_lock())) {
      // printf("cas_ptr failed: %p -> %p\n", old_val, new_val);
      return false;
    }

    help_update(new_val, 0);
    // printf("cas_ptr success: %p -> %p\n", old_val, new_val);
    return true;
  }

  // Load pointer (replica pointer for thread thread_id)
  // Algorithm:
  // (1) If R[i] ends with .0, continue directly
  // (2) If R[i] ends with .1, help update R[i+1] to R[N] as A.1
  // Returns correct ptr (with lock bit cleared)
  T* load_ptr(size_t thread_id) {
    assert(thread_id < replica_num_);
    T* local_r = replica_ptrs_[thread_id].load();

    if (!LockedPointer<T>::has_lock(local_r)) {
      return local_r;
    }

    // printf("load_ptr: %p\n", local_r);

    T* cleared_val = LockedPointer<T>::clear_lock(local_r);
    if (thread_id + 1 < replica_num_) {
      help_update(cleared_val, thread_id + 1);
    }
    // printf("load_ptr cas old: %p, new: %p\n", local_r, cleared_val);
    current_cas(replica_ptrs_[thread_id], local_r, cleared_val);
    // printf("load_ptr success: %p, %p\n", cleared_val, replica_ptrs_[thread_id]);
    return cleared_val;
  }
};

#endif // HELP_UPDATE_H
