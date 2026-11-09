#pragma once
#include "shm/mm.h"
#include "utils/bypass_cache.h"
#include <thread>
#include <utility>
#include <array>
#include <atomic>
#include <type_traits>
#ifndef BWTREE_LOCAL_ONLY
#define BWTREE_LOCAL_ONLY 0
#endif
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
// Ordinary metadata follows node memory (CACHE, or the local RPC partition).
// It must not inherit the NC allocator merely because it contains atomics.
inline MemoryManager& bwtree_metadata_memory() {
  return bwtree_memory();
}

// The owning object stays in CACHE. Only the explicitly selected atomic word
// (or array of atomic words) lives in NC. Both UB and CXL use this layout.
template<class T> struct BwTreeNativeAtomic : std::false_type {};
template<class T> struct BwTreeNativeAtomic<std::atomic<T>> : std::true_type {};
template<class T, size_t N> struct BwTreeNativeAtomic<std::array<T,N>> : BwTreeNativeAtomic<T> {};
template<class T> class BwTreeNCStorage {
  static_assert(!BwTreeNativeAtomic<T>::value, "Native atomics must not be allocated in BwTree NC storage");
  T* const value_;
public:
  template<class... Args> BwTreeNCStorage(Args&&... args)
      : value_(new(shared_atomic_memory(bwtree_memory()).malloc(sizeof(T)))
                   T(std::forward<Args>(args)...)) {}
  ~BwTreeNCStorage() { value_->~T(); shared_atomic_memory(bwtree_memory()).free(value_); }
  BwTreeNCStorage(const BwTreeNCStorage&) = delete;
  BwTreeNCStorage& operator=(const BwTreeNCStorage&) = delete;
  T& get() const { return *value_; }
  template<class... A> auto load(A&&... a) const { return value_->load(std::forward<A>(a)...); }
  template<class... A> void store(A&&... a) { value_->store(std::forward<A>(a)...); }
  template<class... A> auto fetch_add(A&&... a) { return value_->fetch_add(std::forward<A>(a)...); }
  template<class... A> auto compare_exchange_strong(A&&... a) {
    return value_->compare_exchange_strong(std::forward<A>(a)...);
  }
  template<class V> BwTreeNCStorage& operator=(V value) { store(value); return *this; }
  void operator++(int) { value_->fetch_add(1); }
  operator T&() const { return *value_; }
};
inline void bwtree_node_writeback(const void* address, size_t bytes) {
#if BWTREE_LOCAL_ONLY
  (void)address;
  (void)bytes;
#else
  clwb(address, bytes);
#endif
}
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
#if defined(BWTREE_LOCAL_NUMA) || BWTREE_LOCAL_ONLY
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
