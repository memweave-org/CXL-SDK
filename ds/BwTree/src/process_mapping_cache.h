#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
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
  size_t producer = 0;
};

// The producer enumerates allocation ranges into shared NC memory. Receivers
// must never traverse a retired CACHE node: its own replica may already be stale.
struct CachelineRange {
  const void* address;
  size_t bytes;
};
struct CachelineInvalidation {
  const CachelineRange* ranges = nullptr;
  size_t count = 0;
};

// One single-writer broadcast ring per GC worker. Producers do not share a
// tail, reservation CAS or lock, and there is no ordering between their rings.
// Capacity counts batches; payload lives in NC, ordered by published/ACK.
// Producer IDs are the stable GC thread IDs, never shared concurrently.
// 512 covers the coordinated runner's maximum of 192 workers on each host.
template <template<class> class Atomic, size_t Capacity = 16, size_t Processes = 16,
          size_t Producers = 512, size_t BatchSize = 64>
class MappingInvalidationChannel {
  static_assert(Capacity > 0 && Processes > 0 && Processes <= 64 &&
                Producers > 0 && BatchSize > 0, "channel bounds");
  struct Counter {
    Atomic<uint64_t> value{0};
    char padding[64 - sizeof(Atomic<uint64_t>)]{};
  };
  struct Batch {
    size_t count = 0;
    std::array<uint64_t, BatchSize> nodes{};
    std::array<CachelineInvalidation, BatchSize> invalidations{};
  };
  struct Lane {
    Counter published;
    std::array<Counter, Processes> consumers;
    std::array<Batch, Capacity> batches;
  };
  Atomic<uint64_t> active_{0};
  Atomic<uint64_t> producer_count_{1};
  std::array<Atomic<uint64_t>, Processes> registered_{};
  std::array<Lane, Producers> lanes_{};

  // Registration only; never called on the publish/consume data path.
  void membership(size_t process, bool joining) {
    const auto bit = uint64_t{1} << process;
    auto old = active_.load();
    while (!active_.compare_exchange_strong(old, joining ? old | bit : old & ~bit)) {}
  }
public:
  static constexpr size_t batch_size = BatchSize;
  static constexpr size_t producer_limit = Producers;

  // The cache is empty, and local readers start only after attach returns.
  // New readers cannot hold nodes retired before their registration. Start
  // by joining the recipient set BEFORE snapshotting the published heads.
  // Readers still cannot start, so all batches skipped by those snapshots
  // predate this empty cache. Joining first prevents unbounded ring reuse
  // while the snapshots are taken; at most one pre-join batch per lane is
  // still in flight, and its release publication orders the payload.
  void attach(size_t process) {
    if (process >= Processes) throw std::out_of_range("BwTree cache process ID");
    uint64_t expected = 0;
    if (!registered_[process].compare_exchange_strong(expected, 1))
      throw std::logic_error("Duplicate BwTree cache consumer");
    membership(process, true);
    for (auto& lane : lanes_)
      lane.consumers[process].value.store(lane.published.value.load());
  }
  // Local readers and the receiver have stopped. Removing membership also
  // retires ACK obligations of publications racing with this departure.
  void detach(size_t process) {
    if (process >= Processes) throw std::out_of_range("BwTree cache process ID");
    membership(process, false);
    registered_[process].store(0);
  }
  MappingInvalidationTicket try_publish_batch(size_t producer, const uint64_t* ids,
                                              size_t count,
                                              const CachelineInvalidation* invalidations = nullptr) {
    if (producer >= Producers || !count || count > BatchSize || !ids)
      throw std::out_of_range("BwTree invalidation batch/producer bounds");
    // This CAS is a one-time announcement when a new highest producer ID
    // first publishes. Existing lanes use only loads/stores for every batch.
    auto known = producer_count_.load(std::memory_order_acquire);
    while (known <= producer && !producer_count_.compare_exchange_strong(known, producer + 1)) {}
    auto& lane = lanes_[producer];
    const auto head = lane.published.value.load(std::memory_order_relaxed);
    if (head == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("BwTree invalidation sequence exhausted");
    const auto recipients = active_.load(std::memory_order_acquire);
    for (size_t i = 0; i < Processes; ++i)
      if ((recipients & (uint64_t{1} << i)) &&
          head - lane.consumers[i].value.load(std::memory_order_acquire) >= Capacity)
        return {}; // This lane is full; independent producers can still publish.
    auto& batch = lane.batches[head % Capacity];
    batch.count = count;
    for (size_t i = 0; i < count; ++i) {
      batch.nodes[i] = ids[i];
      batch.invalidations[i] = invalidations ? invalidations[i] : CachelineInvalidation{};
    }
    lane.published.value.store(head + 1, std::memory_order_release);
    return {head + 1, recipients, producer};
  }
  // Compatibility for single-node callers; only one caller may own lane 0.
  MappingInvalidationTicket try_publish(uint64_t node_id) {
    return try_publish_batch(0, &node_id, 1);
  }
  // A receiver can defer completion while pre-clear readers leave their epoch.
  // Its batch stays in the ring, and no ACK permits reuse of its descriptors.
  template<class ClearBatch> bool consume_lane_batches(size_t process, size_t producer,
                                                      ClearBatch clear_batch) {
    if (process >= Processes || producer >= Producers)
      throw std::out_of_range("BwTree cache consumer/producer ID");
    auto& lane = lanes_[producer];
    const auto initial = lane.consumers[process].value.load(std::memory_order_relaxed);
    auto ack = initial;
    const auto end = lane.published.value.load(std::memory_order_acquire);
    if (ack == end) return false;
    while (ack < end) {
      const auto& batch = lane.batches[ack % Capacity];
      if (!clear_batch(batch.nodes.data(), batch.invalidations.data(), batch.count)) break;
      ++ack;
    }
    // One release ACK for the completed part of this drain. Pending batches
    // retain their payload until cache maintenance and its fence finish.
    if (ack == initial) return false;
    lane.consumers[process].value.store(ack, std::memory_order_release);
    return true;
  }
  template<class Clear> bool consume_lane(size_t process, size_t producer, Clear clear) {
    return consume_lane_batches(process, producer,
        [&](const uint64_t* nodes, const CachelineInvalidation* invalidations, size_t count) {
      for (size_t i = 0; i < count; ++i) {
        if constexpr (std::is_invocable_v<Clear, uint64_t, CachelineInvalidation>) {
          clear(nodes[i], invalidations[i]);
        } else {
          if (invalidations[i].count)
            throw std::logic_error("Cacheline invalidation requires a range receiver");
          clear(nodes[i]);
        }
      }
      return true;
    });
  }
  size_t producer_count() const { return producer_count_.load(std::memory_order_acquire); }
  template<class Clear> bool consume(size_t process, Clear clear) {
    bool progress = false;
    const auto count = producer_count_.load(std::memory_order_acquire);
    for (size_t producer = 0; producer < count; ++producer)
      progress = consume_lane(process, producer, clear) || progress;
    return progress;
  }
  bool acknowledged(MappingInvalidationTicket ticket) const {
    if (!ticket.sequence) return false;
    if (ticket.producer >= Producers) throw std::out_of_range("BwTree ticket producer ID");
    const auto recipients = ticket.recipients & active_.load(std::memory_order_acquire);
    for (size_t i = 0; i < Processes; ++i)
      if ((recipients & (uint64_t{1} << i)) &&
          lanes_[ticket.producer].consumers[i].value.load(std::memory_order_acquire) < ticket.sequence)
        return false;
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
    std::unique_ptr<DramMappingCache<Table, 1>> table;
    std::function<void(CachelineInvalidation)> invalidate_lines;
    std::function<uint64_t()> begin_grace;
    std::function<bool(uint64_t)> grace_complete;
    std::array<bool, Channel::producer_limit> pending{};
    std::array<uint64_t, Channel::producer_limit> epochs{};
    std::atomic<bool> stopping{false};
    std::thread receiver;
    State(const void* key, Channel& shared, size_t id,
          std::function<void(CachelineInvalidation)> invalidator,
          std::function<uint64_t()> begin, std::function<bool(uint64_t)> complete)
        : owner(key), channel(shared), process(id), invalidate_lines(std::move(invalidator)),
          begin_grace(std::move(begin)), grace_complete(std::move(complete)) {
      if (bool(begin_grace) != bool(grace_complete))
        throw std::logic_error("BwTree invalidation requires both grace callbacks");
      if constexpr (Entries != 0) table = std::make_unique<DramMappingCache<Table, 1>>();
      channel.attach(process);
      try {
        receiver = std::thread([this] {
          while (!stopping.load()) {
            bool progress = false;
            for (size_t producer = 0; producer < channel.producer_count(); ++producer) {
              progress = channel.consume_lane_batches(process, producer,
                  [this, producer](const uint64_t* ids, const CachelineInvalidation* ranges, size_t count) {
                bool has_ranges = false;
                for (size_t i = 0; i < count; ++i) has_ranges |= ranges[i].count != 0;
                if (!pending[producer]) {
                  if constexpr (Entries != 0) {
                    for (size_t i = 0; i < count; ++i) {
                      if (ids[i] >= Entries) std::terminate();
                      (*table)[0][ids[i]].invalidate();
                    }
                  }
                  if (has_ranges && begin_grace) epochs[producer] = begin_grace();
                  pending[producer] = true;
                }
                // A pre-clear reader may refill the old cacheline. Wait before
                // invalidating, rather than ACKing a flush it can undo.
                if (has_ranges && grace_complete && !grace_complete(epochs[producer])) return false;
                for (size_t i = 0; i < count; ++i) if (ranges[i].count) {
                  if (!invalidate_lines) std::terminate();
                  invalidate_lines(ranges[i]); // Must include a completion fence.
                }
                pending[producer] = false;
                return true;
              }) || progress;
            }
            if (!progress) std::this_thread::sleep_for(std::chrono::microseconds(50));
          }
        });
      } catch (...) {
        channel.detach(process);
        throw;
      }
      live.store(true);
      std::fprintf(stderr,
          "BWTREE_MAPPING_CACHE pid=%ld tree=%p address=%p bytes=%zu tables=%zu placement=process-private-dram invalidation=per-worker-batch-ack batch_nodes=64 publish=single-writer fill=busy-slot cachelines=clear-grace-invalidate-ack\n",
          long(getpid()), key, table ? static_cast<void*>(&(*table)[0]) : nullptr,
          Entries ? sizeof(Table) : 0, size_t(Entries != 0));
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
  static void prepare(const void* owner, Channel& channel, size_t process,
                      std::function<void(CachelineInvalidation)> invalidator = {},
                      std::function<uint64_t()> begin_grace = {},
                      std::function<bool(uint64_t)> grace_complete = {}) {
    auto& all = bwtree_process_cache::registry();
    std::lock_guard<std::mutex> guard(all.mutex);
    if (!all.states.count(owner))
      all.states.emplace(owner, std::make_shared<State>(owner, channel, process, std::move(invalidator),
          std::move(begin_grace), std::move(grace_complete)));
  }
  static Table& table(const void* owner) {
    static_assert(Entries != 0, "NOOPT has no software mapping table");
    return (*bound(owner).table)[0];
  }
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
