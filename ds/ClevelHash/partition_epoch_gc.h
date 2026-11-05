#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

// One owner per slot. A guard must cover every use of a borrowed KV pointer,
// including copying a search/update result into the RPC response value.
template<class T> class PartitionEpochGC {
  static constexpr uint64_t inactive = std::numeric_limits<uint64_t>::max();
  static constexpr size_t batch_size = 256;
  struct Batch {
    std::array<T*, batch_size> values;
    size_t size = 0;
    uint64_t epoch = 0;
    Batch* next = nullptr;
  };
  struct alignas(64) Slot {
    std::atomic<uint64_t> active{inactive};
    std::unique_ptr<Batch> local;
  };
  size_t count_;
  std::unique_ptr<Slot[]> slots_;
  std::atomic<uint64_t> epoch_{1};
  std::atomic<Batch*> published_{nullptr};
  Batch* waiting_ = nullptr; // collector only
  void (*destroy_)(T*);
  std::atomic<uint64_t> retired_{0}, reclaimed_{0};
public:
  PartitionEpochGC(size_t count, void (*destroy)(T*))
      : count_(count), slots_(new Slot[count]), destroy_(destroy) {}
  PartitionEpochGC(const PartitionEpochGC&) = delete;
  class Guard {
    PartitionEpochGC& gc_;
    size_t slot_;
  public:
    Guard(PartitionEpochGC& gc, size_t slot) : gc_(gc), slot_(slot) {
      assert(slot < gc_.count_);
      assert(gc_.slots_[slot].active.load() == inactive);
      for (;;) {
        const auto epoch = gc_.epoch_.load();
        gc_.slots_[slot].active.store(epoch);
        if (gc_.epoch_.load() == epoch) break;
      }
    }
    Guard(const Guard&) = delete;
    ~Guard() { gc_.slots_[slot_].active.store(inactive); }
  };
  void retire(size_t slot, T* value) {
    assert(slot < count_ && slots_[slot].active.load() != inactive);
    auto& batch = slots_[slot].local;
    if (!batch) batch.reset(new Batch);
    batch->values[batch->size++] = value;
    if (batch->size == batch_size) flush(slot);
  }
  // Owner calls on exit. An idle owner's unpublished tail is bounded to 255 KVs.
  void flush(size_t slot) {
    auto& local = slots_[slot].local;
    if (!local || !local->size) return;
    Batch* batch = local.release();
    batch->epoch = epoch_.load();
    retired_.fetch_add(batch->size);
    batch->next = published_.load();
    while (!published_.compare_exchange_weak(batch->next, batch)) {}
  }
  // Exactly one background collector; seq_cst entry/epoch scans establish the
  // grace period even for a reader paused between announcing and validation.
  void collect() {
    epoch_.fetch_add(1);
    Batch* batch = published_.exchange(nullptr);
    while (batch) {
      Batch* next = batch->next;
      batch->next = waiting_; waiting_ = batch; batch = next;
    }
    uint64_t minimum = epoch_.load();
    for (size_t i = 0; i < count_; ++i)
      minimum = std::min(minimum, slots_[i].active.load());
    Batch** link = &waiting_;
    while (*link) {
      batch = *link;
      if (batch->epoch < minimum) {
        *link = batch->next;
        for (size_t i = 0; i < batch->size; ++i) destroy_(batch->values[i]);
        reclaimed_.fetch_add(batch->size);
        delete batch;
      } else link = &batch->next;
    }
  }
  uint64_t retired() const { return retired_.load(); }
  uint64_t reclaimed() const { return reclaimed_.load(); }
  // Call only after all owners and the collector have stopped.
  void drain() {
    for (size_t i = 0; i < count_; ++i) {
      assert(slots_[i].active.load() == inactive);
      flush(i);
    }
    collect();
    assert(retired() == reclaimed());
  }
  ~PartitionEpochGC() { drain(); }
};
