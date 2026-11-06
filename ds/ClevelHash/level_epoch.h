#pragma once

#include "shm/mm.h"
#include "utils/atomic_variable.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// One NC cache line per worker; only that worker writes its announcement.
struct alignas(CACHE_LINE_SIZE) ClevelLevelEpochSlot {
  aligned_nt<uint64_t> epoch{0}; // zero means outside an index operation
  ClevelLevelEpochSlot() = default;
  ClevelLevelEpochSlot(const ClevelLevelEpochSlot&) : epoch(0) {}
};

class ClevelLevelEpochGuard {
  const void* domain_;
  ClevelLevelEpochGuard* previous_;
  ClevelLevelEpochSlot* slot_ = nullptr;
  inline static thread_local ClevelLevelEpochGuard* current_ = nullptr;
 public:
  ClevelLevelEpochGuard(const void* domain, const aligned_nt<uint64_t>& global,
                        ClevelLevelEpochSlot& slot)
      : domain_(domain), previous_(current_) {
    // capacity()/expand() can be called inside an already protected operation.
    // Do not overwrite the outer operation's older epoch on nested entry.
    for (auto* guard = previous_; guard; guard = guard->previous_)
      if (guard->domain_ == domain) { current_ = this; return; }
    slot_ = &slot;
    uint64_t epoch;
    do {
      epoch = global.load();
      slot_->epoch.store(epoch);
    } while (global.load() != epoch);
    current_ = this;
  }
  ClevelLevelEpochGuard(const ClevelLevelEpochGuard&) = delete;
  ~ClevelLevelEpochGuard() {
    if (slot_) slot_->epoch.store(0);
    current_ = previous_;
  }
};

// This handle belongs to the attaching process, never to a remote worker.
// The creator holds one; another allocating process holds its own handle.
class ClevelLevelGcWorker {
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
  std::thread thread_;
 public:
  template<class Collect> explicit ClevelLevelGcWorker(Collect collect) {
    thread_ = index_memory_thread([this, collect] {
      std::unique_lock<std::mutex> lock(mutex_);
      while (!wake_.wait_for(lock, std::chrono::milliseconds(10), [this] { return stop_; })) {
        lock.unlock(); collect(); lock.lock();
      }
      lock.unlock(); collect();
    });
  }
  ClevelLevelGcWorker(const ClevelLevelGcWorker&) = delete;
  ~ClevelLevelGcWorker() {
    { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
    wake_.notify_one();
    thread_.join();
  }
};
