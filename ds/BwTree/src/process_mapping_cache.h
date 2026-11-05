#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include "dram_mapping_cache.h"

// Local DRAM only. Hits need one acquire load. The reserved value serializes
// authoritative loads/cache fills with invalidation, without a version word.
class MessageMappingCacheSlot {
  static constexpr uintptr_t busy = 2; // Neither a node address nor its leaf tag.
  std::atomic<uintptr_t> value_{0};
public:
  uintptr_t load() const {
    const auto value = value_.load(std::memory_order_acquire);
    return value == busy ? 0 : value;
  }
  // Fetch MUST read the authoritative mapping here, after acquiring busy. Never
  // pass a previously fetched pointer as the fill source. Losing readers take
  // an uncached snapshot immediately; they neither wait nor publish a cache fill.
  template<class Fetch, class Encode>
  auto fetch_and_fill(Fetch fetch, Encode encode) -> decltype(fetch()) {
    auto previous = value_.load(std::memory_order_relaxed);
    if (previous == busy || !value_.compare_exchange_strong(
            previous, busy, std::memory_order_acquire, std::memory_order_relaxed))
      return fetch();
    try {
      const auto result = fetch();
      const uintptr_t encoded = encode(result);
      if (encoded == busy) throw std::logic_error("Reserved BwTree cache value");
      value_.store(encoded, std::memory_order_release);
      return result;
    } catch (...) {
      value_.store(0, std::memory_order_release);
      throw;
    }
  }
  void invalidate() {
    auto previous = value_.load(std::memory_order_relaxed);
    for (;;) {
      if (previous != busy && value_.compare_exchange_strong(
              previous, busy, std::memory_order_acquire, std::memory_order_relaxed)) break;
      std::this_thread::yield();
      previous = value_.load(std::memory_order_relaxed);
    }
    // A receiver may ACK only after this clear. GC still waits for readers that
    // obtained a pointer before the clear, including uncached losing readers.
    value_.store(0, std::memory_order_release);
  }
};
static_assert(sizeof(MessageMappingCacheSlot) == sizeof(uintptr_t),
              "The local cache must retain pointer-sized entries");
static_assert(std::atomic<uintptr_t>::is_always_lock_free,
              "The local cache requires lock-free pointer atomics");

struct MappingInvalidationTicket {
  uint64_t sequence = 0;
  uint64_t recipients = 0;
};

// Inline in the shared NC tree; contains no process-private addresses. The
// production instantiation uses nt<uint64_t> (native mapped NC atomics on UB).
// Each process consumes its own stream and publishes ACK only after clearing.
template <template<class> class Atomic, size_t Capacity = 1024, size_t Processes = 16>
class MappingInvalidationChannel {
  static_assert(Capacity > 0 && Processes > 0 && Processes <= 64, "channel bounds");
  // Space ACK writers apart without imposing a stronger alignment requirement
  // on the containing tree's existing shared allocator.
  struct Consumer {
    Atomic<uint64_t> ack{0};
    char padding[64 - sizeof(Atomic<uint64_t>)]{};
  };
  Atomic<uint64_t> locked_{0};
  Atomic<uint64_t> published_{0};
  uint64_t active_ = 0; // Accessed only with locked_ held.
  std::array<Atomic<uint64_t>, Capacity> messages_{};
  std::array<Consumer, Processes> consumers_{};

  bool try_lock() {
    uint64_t expected = 0;
    return locked_.compare_exchange_strong(expected, 1);
  }
  void lock() { while (!try_lock()) std::this_thread::yield(); }
  void unlock() { locked_.store(0); }
public:
  // The caller's cache must be empty, and no readers may use it before attach.
  void attach(size_t process) {
    if (process >= Processes) throw std::out_of_range("BwTree cache process ID");
    lock();
    const uint64_t bit = uint64_t{1} << process;
    if (active_ & bit) {
      unlock();
      throw std::logic_error("Duplicate BwTree cache consumer");
    }
    consumers_[process].ack.store(published_.load());
    active_ |= bit;
    unlock();
  }
  // Called after local readers and the receiver have stopped. The retired
  // table can never be reused, so it is safe to acknowledge its entire stream.
  void detach(size_t process) {
    lock();
    consumers_[process].ack.store(published_.load());
    active_ &= ~(uint64_t{1} << process);
    unlock();
  }
  MappingInvalidationTicket try_publish(uint64_t node_id) {
    if (!try_lock()) return {};
    const auto head = published_.load();
    if (head == std::numeric_limits<uint64_t>::max()) {
      unlock();
      throw std::overflow_error("BwTree invalidation sequence exhausted");
    }
    for (size_t i = 0; i < Processes; ++i) {
      if ((active_ & (uint64_t{1} << i)) &&
          head - consumers_[i].ack.load() >= Capacity) {
        unlock();
        return {}; // Backpressure delays GC; it never drops an invalidation.
      }
    }
    messages_[head % Capacity].store(node_id);
    const MappingInvalidationTicket ticket{head + 1, active_};
    published_.store(ticket.sequence);
    unlock();
    return ticket;
  }
  template<class Clear> bool consume(size_t process, Clear clear) {
    auto ack = consumers_[process].ack.load();
    const auto end = published_.load();
    if (ack == end) return false;
    while (ack < end) {
      clear(messages_[ack % Capacity].load());
      consumers_[process].ack.store(++ack);
    }
    return true;
  }
  bool acknowledged(MappingInvalidationTicket ticket) const {
    if (!ticket.sequence) return false;
    for (size_t i = 0; i < Processes; ++i)
      if ((ticket.recipients & (uint64_t{1} << i)) &&
          consumers_[i].ack.load() < ticket.sequence) return false;
    return true;
  }
};

namespace bwtree_process_cache {
struct Service {
  std::atomic<bool> live{false};
  virtual void stop() = 0;
  virtual ~Service() = default;
};
struct Registry {
  std::mutex mutex;
  std::unordered_map<const void*, std::shared_ptr<Service>> states;
};
inline Registry& registry() { static Registry value; return value; }

// Both A and B call this before unmapping the shared NC region, including A
// which never invokes the shared tree's destructor.
inline void shutdown() {
  auto& all = registry();
  std::lock_guard<std::mutex> guard(all.mutex);
  for (auto& item : all.states) item.second->stop();
  all.states.clear();
}
} // namespace bwtree_process_cache

// One table per process and tree, shared by its local worker threads. Owning
// pointers, receiver thread and registry all live outside the shared tree.
template <size_t Entries, template<class> class Atomic> class ProcessMappingCache {
public:
  using Table = std::array<MessageMappingCacheSlot, Entries>;
  using Channel = MappingInvalidationChannel<Atomic>;
private:
  struct State : bwtree_process_cache::Service {
    const void* owner;
    Channel& channel;
    size_t process;
    DramMappingCache<Table, 1> table;
    std::atomic<bool> stopping{false};
    std::thread receiver;
    State(const void* key, Channel& shared, size_t id)
        : owner(key), channel(shared), process(id) {
      channel.attach(process);
      try {
        receiver = std::thread([this] {
          while (!stopping.load()) {
            if (!channel.consume(process, [this](uint64_t id) {
                  if (id >= Entries) std::terminate();
                  table[0][id].invalidate();
                })) std::this_thread::sleep_for(std::chrono::microseconds(50));
          }
        });
      } catch (...) {
        channel.detach(process);
        throw;
      }
      live.store(true);
      std::fprintf(stderr,
          "BWTREE_MAPPING_CACHE pid=%ld tree=%p address=%p bytes=%zu tables=1 placement=process-private-dram invalidation=message-ack fill=busy-slot\n",
          long(getpid()), key, static_cast<void*>(&table[0]), sizeof(Table));
    }
    void stop() override {
      if (!live.exchange(false)) return;
      stopping.store(true);
      receiver.join();
      channel.detach(process);
    }
    ~State() override { stop(); }
  };
  static std::shared_ptr<State>& binding() {
    static thread_local std::shared_ptr<State> value;
    return value;
  }
  static State& bound(const void* owner) {
    auto& local = binding();
    if (!local || local->owner != owner || !local->live.load()) {
      auto& all = bwtree_process_cache::registry();
      std::lock_guard<std::mutex> guard(all.mutex);
      auto found = all.states.find(owner);
      if (found == all.states.end())
        throw std::logic_error("BwTree cache used before process registration");
      local = std::dynamic_pointer_cast<State>(found->second);
      if (!local) throw std::logic_error("BwTree cache type mismatch");
    }
    return *local;
  }
public:
  static void prepare(const void* owner, Channel& channel, size_t process) {
    auto& all = bwtree_process_cache::registry();
    std::lock_guard<std::mutex> guard(all.mutex);
    if (!all.states.count(owner))
      all.states.emplace(owner, std::make_shared<State>(owner, channel, process));
  }
  static Table& table(const void* owner) { return bound(owner).table[0]; }
  static uintptr_t load(const void* owner, size_t index) {
    return table(owner)[index].load();
  }
  static void release(const void* owner) {
    auto& all = bwtree_process_cache::registry();
    std::lock_guard<std::mutex> guard(all.mutex);
    auto found = all.states.find(owner);
    if (found != all.states.end()) {
      found->second->stop();
      all.states.erase(found);
    }
    auto& local = binding();
    if (local && local->owner == owner) local.reset();
  }
};
