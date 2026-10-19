#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "utils/atomic_pointer.h"
#include "utils/atomic_variable.h"
#include "utils/bypass_cache.h"
#include "utils/compiler.h"
#include "utils/config.h"
#include "utils/timing.h"

/*
 * If NO_CC is enabled, only objects without any external pointer
 * or reference can be put into this queue.
 */
#ifdef USE_NO_CC_QUEUE
template <typename T>
class MPMCQueue {
 public:
  MPMCQueue(size_t capacity) : capacity(capacity), mask(capacity - 1) {
    buffer.allocate(capacity);
    assert((capacity & (capacity - 1)) == 0);
    buffer.flush_elements(capacity);
    clwb(this, sizeof(*this));
  }

  MPMCQueue(MPMCQueue&& other) noexcept
      : capacity(other.capacity),
        mask(other.mask),
        head(other.head.load()),
        tail(other.tail.load()),
        buffer(std::move(other.buffer)) {
    other.capacity = 0;
    other.mask = 0;
    other.head = 0;
    other.tail = 0;
    clwb(this, sizeof(*this));
  }

  MPMCQueue& operator=(MPMCQueue&& other) noexcept {
    if (this != &other) {
      capacity = other.capacity;
      mask = other.mask;
      head = other.head.load();
      tail = other.tail.load();
      buffer = std::move(other.buffer);
      other.capacity = 0;
      other.mask = 0;
    }
    clwb(this, sizeof(*this));
    return *this;
  }

  MPMCQueue(const MPMCQueue&) = delete;
  MPMCQueue& operator=(const MPMCQueue&) = delete;

  ~MPMCQueue() = default;

  template <typename U>
  bool enqueue(U&& item) {
#ifdef QUEUE_LEN_PERF
    len_stat.Log(tail.load() - head.load());
#endif
    size_t current_tail = tail.fetch_add(1);
    auto& slot = buffer[idx(current_tail)];
    // Ensure the slot is empty
    while (turn(current_tail) * 2 != slot.turn.load(std::memory_order_acquire));
    // Use std::forward to perfectly forward the item to the buffer
    slot.value = std::forward<U>(item);
    clwb(&slot.value, sizeof(T));
    slot.turn.store(turn(current_tail) * 2 + 1, std::memory_order_release);
    return true;
  }

  /*
   * This function may get into an infinite loop waiting for the next element,
   * if a client put an object into the queue and fails.
   */
  bool dequeue(T& item) {
#ifdef QUEUE_LATENCY
    TimerAverage timer("queue_latency");
    timer.Start();
#endif
#ifdef QUEUE_LATENCY_BREAKDOWN
    TimerAverage timer_poll("queue_latency_poll");
    timer_poll.Start();
#endif
    size_t current_head = head.fetch_add(1);
    auto& slot = buffer[idx(current_head)];
#if defined(USE_MWAIT) && defined(__x86_64__) && defined(__WAITPKG__)
    while (turn(current_head) * 2 + 1 !=
           slot.turn.load(std::memory_order_acquire)) {
      _umonitor(&slot.turn);
      if (turn(current_head) * 2 + 1 !=
          slot.turn.load(std::memory_order_acquire)) {
        _umwait(0, 1);
      }
    }
#else
    while (turn(current_head) * 2 + 1 !=
           slot.turn.load(std::memory_order_acquire));
#endif
#ifdef QUEUE_LATENCY_BREAKDOWN
    timer_poll.Stop();
    TimerAverage timer_dequeue("queue_latency_dequeue");
    timer_dequeue.Start();
#endif
    clflush(&slot.value, sizeof(T));
    item = std::move(slot.value);
    slot.turn.store(turn(current_head) * 2 + 2, std::memory_order_release);
#ifdef QUEUE_LATENCY_BREAKDOWN
    timer_dequeue.Stop();
#endif
#ifdef QUEUE_LATENCY
    timer.Stop();
#endif
    return true;
  }

  // Exit when there is no element in the next slot, never get into infinite
  // loop
  bool try_dequeue(T& item) {
#ifdef QUEUE_LATENCY
    TimerAverage timer("queue_latency");
    timer.Start();
#endif
    size_t current_head = head.load(std::memory_order_acquire);
    for (;;) {
      auto& slot = buffer[idx(current_head)];
      if (turn(current_head) * 2 + 1 ==
          slot.turn.load(std::memory_order_acquire)) {
        if (head.compare_exchange_strong(current_head, current_head + 1)) {
          clflush(&slot.value, sizeof(T));
          item = std::move(slot.value);
          slot.turn.store(turn(current_head) * 2 + 2,
                          std::memory_order_release);
          #ifdef QUEUE_LATENCY
              timer.Stop();
          #endif
          return true;
        }
      } else {
        auto const prev_head = current_head;
        current_head = head.load(std::memory_order_acquire);
        if (current_head == prev_head) {
          #ifdef QUEUE_LATENCY
              timer.Stop();
          #endif
          return false;
        }
      }
    }
#ifdef QUEUE_LATENCY
    timer.Stop();
#endif
    return true;
  }

  /*
   * Whether there is no element in the queue at all.
   */
  bool empty() const { return head.load() == tail.load(); }

  size_t size() const { return tail.load() - head.load(); }

 private:
  size_t capacity;
  size_t mask;

  struct alignas(64) T_wrapper {
    T value;
    nt<size_t> turn{0};
  };

  nt<size_t> head{0};
  nt<size_t> tail{0};
  alignas(64) nt_pointer<T_wrapper[]> buffer;
#ifdef QUEUE_LEN_PERF
  Logger<size_t> len_stat{std::string("queue_len")};
#endif
  constexpr size_t idx(size_t i) const noexcept { return i % capacity; }
  constexpr size_t turn(size_t i) const noexcept { return i / capacity; }
};
#else
template <typename T>
class MPMCQueue {
 public:
  MPMCQueue(size_t capacity)
      : capacity(capacity),
        mask(capacity - 1),
        buffer(new T_wrapper[capacity]) {
    assert((capacity & (capacity - 1)) == 0);
  }

  MPMCQueue(MPMCQueue&& other) noexcept
      : capacity(other.capacity),
        mask(other.mask),
        buffer(std::move(other.buffer)),
        head(other.head.load(std::memory_order_relaxed)),
        tail(other.tail.load(std::memory_order_relaxed)) {
    other.capacity = 0;
    other.mask = 0;
    other.head = 0;
    other.tail = 0;
  }

  MPMCQueue& operator=(MPMCQueue&& other) noexcept {
    if (this != &other) {
      capacity = other.capacity;
      mask = other.mask;
      buffer = std::move(other.buffer);
      head = other.head.load(std::memory_order_relaxed);
      tail = other.tail.load(std::memory_order_relaxed);
      other.capacity = 0;
      other.mask = 0;
    }
    return *this;
  }

  MPMCQueue(const MPMCQueue&) = delete;
  MPMCQueue& operator=(const MPMCQueue&) = delete;

  ~MPMCQueue() = default;

  // Fail when the queue is full
  template <typename U>
  bool enqueue(U&& item) noexcept {
#ifdef QUEUE_LEN_PERF
    len_stat.Log(tail.load() - head.load());
#endif
    size_t current_tail = tail.fetch_add(1);
    auto& slot = buffer[idx(current_tail)];
    // Ensure the slot is empty
    while (turn(current_tail) * 2 != slot.turn.load(std::memory_order_acquire));
    // Use std::forward to perfectly forward the item to the buffer
    slot.value = std::forward<U>(item);
    slot.turn.store(turn(current_tail) * 2 + 1, std::memory_order_release);
    return true;
  }

  /*
   * This function may get into an infinite loop waiting for the next element,
   * if a client put an object into the queue and fails.
   */
  bool dequeue(T& item) noexcept {
#ifdef QUEUE_LATENCY
    TimerAverage timer("queue_latency");
    timer.Start();
#endif
#ifdef QUEUE_LEN_PERF
    len_stat.Log(tail.load() - head.load());
#endif
#ifdef QUEUE_LATENCY_BREAKDOWN
    TimerAverage timer_poll("queue_latency_poll");
    timer_poll.Start();
#endif
    size_t current_head = head.fetch_add(1);
    auto& slot = buffer[idx(current_head)];
    while (turn(current_head) * 2 + 1 !=
           slot.turn.load(std::memory_order_acquire));
#ifdef QUEUE_LATENCY_BREAKDOWN
    timer_poll.Stop();
    TimerAverage timer_dequeue("queue_latency_dequeue");
    timer_dequeue.Start();
#endif
    item = std::move(slot.value);
    slot.turn.store(turn(current_head) * 2 + 2, std::memory_order_release);
#ifdef QUEUE_LATENCY_BREAKDOWN
    timer_dequeue.Stop();
#endif
#ifdef QUEUE_LATENCY
    timer.Stop();
#endif
    return true;
  }

  // Exit when there is no element in the next slot, never get into infinite
  // loop
  bool try_dequeue(T& item) noexcept {
#ifdef QUEUE_LATENCY
    TimerAverage timer("queue_latency");
    timer.Start();
#endif
#ifdef QUEUE_LEN_PERF
    len_stat.Log(tail.load() - head.load());
#endif
    size_t current_head = head.load(std::memory_order_acquire);
    for (;;) {
      auto& slot = buffer[idx(current_head)];
      if (turn(current_head) * 2 + 1 ==
          slot.turn.load(std::memory_order_acquire)) {
        if (head.compare_exchange_strong(current_head, current_head + 1)) {
          item = std::move(slot.value);
          slot.turn.store(turn(current_head) * 2 + 2,
                          std::memory_order_release);
          return true;
        }
      } else {
        auto const prev_head = current_head;
        current_head = head.load(std::memory_order_acquire);
        if (current_head == prev_head) {
          return false;
        }
      }
    }
#ifdef QUEUE_LATENCY
    timer.Stop();
#endif
    return true;
  }

  /// Returns the number of elements in the queue.
  /// The size can be negative when the queue is empty and there is at least one
  /// reader waiting. Since this is a concurrent queue the size is only a best
  /// effort guess until all reader and writer threads have been joined.
  ptrdiff_t size() const noexcept {
    // TODO: How can we deal with wrapped queue on 32bit?
    return static_cast<ptrdiff_t>(tail.load(std::memory_order_relaxed) -
                                  head.load(std::memory_order_relaxed));
  }

  /*
   * Whether there is no element in the queue at all.
   */
  bool empty() const noexcept { return size() <= 0; }

 private:
  struct alignas(64) T_wrapper {
    T value;
    std::atomic<size_t> turn = {0};
  };
  size_t capacity;
  size_t mask;
  std::unique_ptr<T_wrapper[]> buffer;

  // The position where you should find the next element to pop
  alignas(64) std::atomic<size_t> head{0};
  // The position where you can put in a new element
  alignas(64) std::atomic<size_t> tail{0};
#ifdef QUEUE_LEN_PERF
  Logger<size_t> len_stat{std::string("queue_len")};
#endif
  constexpr size_t idx(size_t i) const noexcept { return i % capacity; }
  constexpr size_t turn(size_t i) const noexcept { return i / capacity; }
};
#endif