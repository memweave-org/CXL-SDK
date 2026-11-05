#pragma once
#include "shm/mm.h"
#include <thread>
#include <utility>
#ifdef BWTREE_LOCAL_NUMA
#include <pthread.h>
#include <stdexcept>
struct BwTreeAllocationContext {
  MemoryManager* memory;
  int node;
  cpu_set_t cpus;
};
inline thread_local BwTreeAllocationContext* bwtree_allocation_context = nullptr;
inline MemoryManager& bwtree_memory() {
  if (!bwtree_allocation_context) throw std::logic_error("Missing BwTree allocation context");
  return *bwtree_allocation_context->memory;
}
#else
inline MemoryManager& bwtree_memory() { return cacheable; }
#endif
// GC threads must inherit the partition allocator, including future allocations.
template<class F> std::thread bwtree_background_thread(F work) {
#ifdef BWTREE_LOCAL_NUMA
  auto* context = bwtree_allocation_context;
  if (!context) throw std::logic_error("Missing BwTree background context");
  return index_memory_thread([context, work=std::move(work)]() mutable {
    bwtree_allocation_context = context;
    if (pthread_setaffinity_np(pthread_self(), sizeof(context->cpus), &context->cpus)) std::abort();
    work();
  });
#else
  return index_memory_thread(std::move(work));
#endif
}
#ifdef BWTREE_LOCAL_NUMA
// Store the owner: container destruction need not occur on its allocating thread.
template<class T> class BwTreeAllocator {
  template<class> friend class BwTreeAllocator;
  MemoryManager* memory_;
public:
  using value_type=T;
  BwTreeAllocator(): memory_(&bwtree_memory()) {}
  template<class U> BwTreeAllocator(const BwTreeAllocator<U>& other): memory_(other.memory_) {}
  T* allocate(size_t n) {
    if (n > std::numeric_limits<size_t>::max()/sizeof(T)) throw std::bad_alloc();
    auto* p=static_cast<T*>(memory_->malloc(n*sizeof(T)));
    if (!p) throw std::bad_alloc();
    return p;
  }
  void deallocate(T* p,size_t) { memory_->free(p); }
  template<class U> bool operator==(const BwTreeAllocator<U>& other) const { return memory_==other.memory_; }
  template<class U> bool operator!=(const BwTreeAllocator<U>& other) const { return !(*this==other); }
};
#else
template<class T> using BwTreeAllocator=CXLAllocator<T>;
#endif
