// Standalone fixed-pool contract; no SDK or UB hardware required.
#include "shm/ub_arena.h"
#include <sys/mman.h>
#include <array>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

static void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

int main() {
  constexpr size_t capacity = 64ul << 20;
  auto* mapping = static_cast<char*>(mmap(nullptr, capacity, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  require(mapping != MAP_FAILED, "mmap failed");
  {
    UbArena arena;
    arena.configure(mapping + 64, capacity - 64);
    auto check = [&](void* p, size_t bytes, size_t alignment) {
      require(uintptr_t(p) % alignment == 0, "absolute alignment lost");
      require(static_cast<char*>(p) >= arena.base &&
              static_cast<char*>(p) + bytes <= mapping + capacity, "allocation escaped slice");
    };
    // Ordinary malloc, explicit alignment, overflow rejection, and recovery.
    auto* small = arena.allocate(7);
    auto* aligned = arena.allocate(4097, 4096);
    check(small, 7, alignof(void*)); check(aligned, 4097, 4096);
    require(small != aligned, "live allocations alias");
    bool failed = false;
    try { arena.allocate(SIZE_MAX, 64); } catch (const std::bad_alloc&) { failed = true; }
    require(failed, "overflow accepted");
    failed = false;
    try { arena.allocate(64, 3); } catch (const std::bad_alloc&) { failed = true; }
    require(failed, "invalid alignment accepted");
    arena.deallocate(small); arena.deallocate(aligned);
    failed = false;
    try { arena.deallocate(mapping); } catch (const std::runtime_error&) { failed = true; }
    require(failed, "out-of-slice free accepted");

    // Allocator metadata must stay outside CACHE/NC object storage. With the
    // default memkind settings, allocating/freeing objects does not write it.
    auto* retired = arena.allocate(8192, 64);
    std::memset(retired, 0x5a, 8192);
    require(mprotect(mapping, capacity, PROT_READ) == 0, "mprotect failed");
    arena.deallocate(retired);
    auto* replacement = arena.allocate(8192, 64);
    arena.deallocate(replacement);
    require(mprotect(mapping, capacity, PROT_READ | PROT_WRITE) == 0, "restore failed");

    // Concurrent live objects must not overlap; another thread must be able
    // to reclaim them after their allocating thread has exited (epoch GC).
    constexpr size_t workers = 8, slots = 32;
    std::array<std::array<void*, slots>, workers> handoff{};
    std::vector<std::thread> threads;
    for (size_t t = 0; t < workers; ++t) threads.emplace_back([&, t] {
      for (size_t round = 0; round < 256; ++round) {
        for (size_t i = 0; i < slots; ++i) {
          const size_t bytes = 64 * (i + 1);
          auto* p = arena.allocate(bytes, 64);
          check(p, bytes, 64);
          std::memset(p, int(t + 1), bytes);
          handoff[t][i] = p;
        }
        std::this_thread::yield();
        for (size_t i = 0; i < slots; ++i) {
          auto* p = static_cast<unsigned char*>(handoff[t][i]);
          for (size_t j = 0; j < 64 * (i + 1); ++j)
            require(p[j] == t + 1, "concurrent allocations overlap");
          if (round != 255) arena.deallocate(p);
        }
      }
    });
    for (auto& thread : threads) thread.join();
    for (auto& batch : handoff) for (void* p : batch) arena.deallocate(p);

    // Exhaustion must never fall back to process-private heap. Freed large
    // blocks must sustain a second pass in the same bounded pool. jemalloc
    // may vary the cache-line offset within a reused large extent.
    arena.configure(mapping + 64, capacity - 64);
    std::vector<void*> blocks;
    try {
      for (;;) {
        auto* p = arena.allocate(1ul << 20, 64);
        check(p, 1ul << 20, 64);
        blocks.push_back(p);
        require(blocks.size() < 64, "pool exhaustion fell back to heap");
      }
    } catch (const std::bad_alloc&) {}
    require(!blocks.empty(), "pool unusable");
    const auto count = blocks.size();
    for (void* p : blocks) arena.deallocate(p);
    blocks.clear();
    for (size_t i = 0; i < count; ++i) {
      auto* p = arena.allocate(1ul << 20, 64);
      check(p, 1ul << 20, 64);
      blocks.push_back(p);
    }
    for (void* p : blocks) arena.deallocate(p);

    arena.configure(mapping + 64, capacity - 64, false);
    auto* first = arena.allocate(64, 64); arena.deallocate(first);
    require(arena.allocate(64, 64) != first, "immutable-address mode reused memory");
    arena.configure(nullptr, 0);
    require(!arena.base && !arena.size, "reset retained backing mapping");
  }
  require(munmap(mapping, capacity) == 0, "munmap failed");
  std::cout << "PASS UB memkind bounds, alignment, external metadata, cross-thread free, exhaustion/reuse and reset\n";
}
