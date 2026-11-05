#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <new>
#include <stdexcept>
#include <unordered_map>

// Allocator bookkeeping is process-local. In particular, a free must never
// write a next pointer into a CACHE object still cached by another host.
// Each process owns a disjoint arena slice; remote frees are rejected.
class UbArena {
 public:
  char* base{};
  size_t size{};
  std::atomic<size_t> used{0}; // High-water mark, including alignment gaps.

  // Configure/reset only while application threads are stopped.
  void configure(void* address, size_t bytes, bool recycle = true) {
    std::lock_guard<std::mutex> guard(mutex_);
    free_.clear(); allocated_.clear(); live_ = 0;
    if (bytes && recycle) free_.emplace(0, bytes);
    base = static_cast<char*>(address); size = bytes;
    recycle_ = recycle; used.store(0);
  }

  void* allocate(size_t bytes, size_t alignment) {
    if (!base || !alignment || (alignment & (alignment - 1)) || bytes > SIZE_MAX - 63)
      throw std::bad_alloc();
    alignment = std::max(alignment, size_t(64));
    const size_t padded = (std::max(bytes, size_t(1)) + 63) & ~size_t(63);
    if (!recycle_) {
      auto old = used.load(std::memory_order_relaxed);
      for (;;) {
        size_t start;
        if (!fit(old, size - old, padded, alignment, start)) throw std::bad_alloc();
        if (used.compare_exchange_weak(old, start + padded, std::memory_order_relaxed))
          return base + start;
      }
    }
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto it = free_.begin(); it != free_.end(); ++it) {
      size_t start;
      if (!fit(it->first, it->second, padded, alignment, start)) continue;
      const size_t end = start + padded, limit = it->first + it->second;
      void* result = base + start;
      auto record = allocated_.emplace(result, padded);
      if (!record.second) throw std::logic_error("UB allocation overlaps a live block");
      try {
        if (end < limit) free_.emplace(end, limit - end);
      } catch (...) {
        allocated_.erase(record.first);
        throw;
      }
      if (start != it->first) it->second = start - it->first;
      else free_.erase(it);
      live_ += padded;
      used.store(std::max(used.load(std::memory_order_relaxed), end), std::memory_order_relaxed);
      return result;
    }
    throw std::bad_alloc();
  }

  void deallocate(void* pointer) {
    if (!pointer) return;
    const auto address = uintptr_t(pointer), begin = uintptr_t(base);
    if (!base || address < begin || address - begin >= size)
      throw std::runtime_error("UB free outside this process's arena");
    if (!recycle_) return; // Explicit immutable-address lifetime policy.
    std::lock_guard<std::mutex> guard(mutex_);
    auto allocation = allocated_.find(pointer);
    if (allocation == allocated_.end())
      throw std::runtime_error("UB free requires a live allocation start (invalid or double free)");
    const size_t offset = address - begin, bytes = allocation->second;
    auto next = free_.lower_bound(offset);
    auto previous = next == free_.begin() ? free_.end() : std::prev(next);
    if (previous != free_.end() && previous->first + previous->second == offset) {
      previous->second += bytes;
      if (next != free_.end() && offset + bytes == next->first) {
        previous->second += next->second;
        free_.erase(next);
      }
    } else if (next != free_.end() && offset + bytes == next->first) {
      auto node = free_.extract(next);
      node.key() = offset; node.mapped() += bytes;
      free_.insert(std::move(node));
    } else {
      free_.emplace(offset, bytes);
    }
    allocated_.erase(allocation);
    live_ -= bytes;
  }

  size_t live_bytes() {
    if (!recycle_) return used.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(mutex_);
    return live_;
  }

 private:
  bool fit(size_t offset, size_t available, size_t bytes, size_t alignment, size_t& start) const {
    // Align the absolute address: an NC slice need only be 64-byte aligned.
    const auto address = uintptr_t(base) + offset;
    const size_t padding = (-address) & (alignment - 1);
    if (padding > available || bytes > available - padding) return false;
    start = offset + padding;
    return true;
  }
  bool recycle_{true};
  std::mutex mutex_;
  std::map<size_t, size_t> free_;
  std::unordered_map<void*, size_t> allocated_;
  size_t live_{};
};
