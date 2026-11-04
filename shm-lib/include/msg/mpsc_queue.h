#pragma once
#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include "utils/atomic_pointer.h"
#include "utils/atomic_variable.h"
#include "utils/bypass_cache.h"

// Bounded CXL queue with multiple producers and exactly one consumer.
// A producer reserves a ticket, waits for the previous generation to be freed,
// writes the payload, then publishes it. A stalled producer can delay FIFO
// progress. No consumer migration or concurrent dequeue is supported.
// This keeps the MPMC slot/ticket protocol and cache maintenance; only the
// consumer's head is private state instead of a shared atomic/RMW location.
template<class T> class MPSCQueue {
  static_assert(std::is_trivially_copyable<T>::value,
                "CXL request payloads must be trivially copyable");
 public:
  explicit MPSCQueue(size_t capacity):capacity_(capacity),mask_(capacity-1) {
    if(!capacity || (capacity & (capacity-1)))
      throw std::invalid_argument("MPSC capacity must be a nonzero power of two");
    buffer_.allocate(capacity);
    buffer_.flush_elements(capacity);
    clwb(this,sizeof(*this));
  }
  // Placement diagnostics only; never used on the enqueue/dequeue hot path.
  const void* storage_address() { return buffer_.load(); }
  MPSCQueue(const MPSCQueue&)=delete;
  MPSCQueue& operator=(const MPSCQueue&)=delete;
  // One bounded reservation attempt. False means full OR producer contention;
  // no ticket was claimed and the caller may safely try a different queue.
  // After a successful CAS we must publish: abandoning a ticket leaves a hole.
  template<class U> bool try_enqueue(U&& item) {
    size_t ticket=tail_.load();
    auto& slot=buffer_[ticket % capacity_];
    const size_t generation=ticket/capacity_;
    if(slot.turn.load(std::memory_order_acquire)!=generation*2) return false;
    if(!tail_.compare_exchange_strong(ticket,ticket+1)) return false;
    slot.value=std::forward<U>(item);
    clwb(&slot.value,sizeof(T));
    slot.turn.store(generation*2+1,std::memory_order_release);
    return true;
  }
  template<class U> bool enqueue(U&& item) {
    const size_t ticket=tail_.fetch_add(1);
    auto& slot=buffer_[ticket % capacity_];
    const size_t generation=ticket/capacity_;
    while(slot.turn.load(std::memory_order_acquire)!=generation*2) {}
    slot.value=std::forward<U>(item);
    clwb(&slot.value,sizeof(T));
    slot.turn.store(generation*2+1,std::memory_order_release);
    return true;
  }
  // Only the consumer reads/writes head_. Producers reclaim space through
  // slot.turn, so neither publication of head_ nor CAS on head_ is needed.
  bool try_dequeue(T& item) {
    const size_t ticket=head_;
    auto& slot=buffer_[ticket % capacity_];
    const size_t generation=ticket/capacity_;
    if(slot.turn.load(std::memory_order_acquire)!=generation*2+1) return false;
    clflush(&slot.value,sizeof(T));
    item=slot.value;
    slot.turn.store(generation*2+2,std::memory_order_release);
    head_=ticket+1;
    return true;
  }
 private:
  size_t capacity_,mask_;
  struct alignas(64) Slot {
    T value;
    nt<size_t> turn{0};
  };
  // Same cache-line separation as the MPMC queue; one consumer owns this line.
  alignas(64) size_t head_=0;
  nt<size_t> tail_{0};
  alignas(64) nt_pointer<Slot[]> buffer_;
};
