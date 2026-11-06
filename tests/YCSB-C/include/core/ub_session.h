#pragma once
#ifdef YCSB_UB_TWO_HOST
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include "core/properties.h"
namespace ycsbc_ub {
void* allocate_local_dram(size_t bytes);
void free_local_dram(void* address, size_t bytes) noexcept;
void check_local_workload(const void* address, size_t bytes, unsigned thread, size_t operations);
// Workload descriptors are private to a worker, unlike index values and NT state.
template<class T> struct LocalDramAllocator {
  using value_type = T;
  LocalDramAllocator() = default;
  template<class U> LocalDramAllocator(const LocalDramAllocator<U>&) {}
  T* allocate(size_t n) {
    if (n > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_alloc();
    return static_cast<T*>(allocate_local_dram(n * sizeof(T)));
  }
  void deallocate(T* p, size_t n) noexcept { free_local_dram(p, n * sizeof(T)); }
  template<class U> bool operator==(const LocalDramAllocator<U>&) const { return true; }
  template<class U> bool operator!=(const LocalDramAllocator<U>&) const { return false; }
};
void stage(const std::string& message);
void reply(const std::string& message);
int threads(int machine); // B is native master 0, A is follower 1.
int first_thread(int machine);
int cpu_start();
int rpc_worker_cpu(unsigned local_index);
int worker_cpu(unsigned local_index);
bool nc_data();
bool nc_direct_data();
bool rpc_mode();
bool in_local_index(const void* address, size_t bytes = 1);
void prepare(utils::Properties& props);
void* control_header();
void ready();
void await_cleanup();
void finish();
}
#endif
