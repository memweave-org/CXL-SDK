#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memkind.h>
#include <new>
#include <stdexcept>
#include <string>

// Use the same fixed-pool allocator as CXL. The kind and its allocator metadata
// belong to this process; each process owns a disjoint slice of the mapping.
// Reclamation still requires the caller's reader/epoch grace period.
class UbArena {
 public:
  char* base{};
  size_t size{};

  UbArena() = default;
  UbArena(const UbArena&) = delete;
  UbArena& operator=(const UbArena&) = delete;
  ~UbArena() { if (pool_) memkind_destroy_kind(pool_); }

  // Configure/reset only while application threads are stopped. Destroy the
  // old kind before giving up ownership of, or unmapping, its backing memory.
  void configure(void* address, size_t bytes, bool recycle = true) {
    if (pool_) {
      const int rc = memkind_destroy_kind(pool_);
      if (rc) throw std::runtime_error("UB memkind_destroy_kind failed: " + std::to_string(rc));
      pool_ = nullptr;
    }
    base = nullptr; size = 0; cursor_.store(0);
    recycle_ = recycle;
    if (bytes && recycle) {
      const int rc = memkind_create_fixed(address, bytes, &pool_);
      if (rc) throw std::runtime_error("UB memkind_create_fixed failed: " + std::to_string(rc));
    }
    base = static_cast<char*>(address); size = bytes;
  }

  // alignment=0 is ordinary malloc, exactly as in the CXL MemoryManager.
  // Explicit posix_memalign/clalign requests retain their requested alignment.
  void* allocate(size_t bytes, size_t alignment = 0) {
    if (!base) throw std::bad_alloc();
    if (!recycle_) {
      // This cursor is allocation state for the optional bump allocator, not
      // telemetry. The normal memkind path never reads or updates it.
      if (!alignment) alignment = alignof(std::max_align_t);
      if (alignment & (alignment - 1)) throw std::bad_alloc();
      const size_t requested = std::max(bytes, size_t(1));
      auto old = cursor_.load(std::memory_order_relaxed);
      for (;;) {
        size_t start;
        if (!fit(old, size - old, requested, alignment, start)) throw std::bad_alloc();
        if (cursor_.compare_exchange_weak(old, start + requested, std::memory_order_relaxed))
          return base + start;
      }
    }
    void* result = nullptr;
    if (alignment) {
      if (memkind_posix_memalign(pool_, &result, alignment, bytes)) throw std::bad_alloc();
    } else {
      result = memkind_malloc(pool_, bytes);
    }
    if (!result) throw std::bad_alloc();
    return result;
  }

  void deallocate(void* pointer) {
    if (!pointer) return;
    const auto address = uintptr_t(pointer), begin = uintptr_t(base);
    if (!base || address < begin || address - begin >= size)
      throw std::runtime_error("UB free outside this process's arena");
    if (!recycle_) return; // Explicit immutable-address lifetime policy.
    // As in CXL/memkind, the caller must supply a live allocation start.
    // Interior pointers and double frees are allocator misuse, not validated
    // with a second, globally locked allocation registry.
    memkind_free(pool_, pointer);
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
  memkind_t pool_{};
  std::atomic<size_t> cursor_{0}; // Used only when recycle=false.
};
