// Small, untimed allocator/ClevelHash assertions. Requires the file-backed SDK
// fixture; this is neither a performance test nor a non-coherent cache model.
#include "shm/mm.h"
#include "clevel_hash.hpp"
#include <unistd.h>
#include <array>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template<class F> void rejected(F action) {
  bool failed = false;
  try { action(); } catch (const std::runtime_error&) { failed = true; }
  require(failed, "invalid free was accepted");
}
void allocator_contract(bool atomic, char* region) {
  MemoryManager manager(atomic);
  // Exercise alignment relative to an arena whose base is not page aligned.
  ub_configure_arena(atomic, region + 64, 128 * 1024);
  manager.free(nullptr);
  void *aligned = nullptr;
  require(manager.posix_memalign(&aligned, 4096, 4096) == 0 && uintptr_t(aligned) % 4096 == 0,
          "absolute alignment was lost");
  auto* other = manager.malloc(64);
  rejected([&] { manager.free(static_cast<char*>(aligned) + 64); });
  rejected([&] { MemoryManager(!atomic).free(aligned); });
  manager.free(aligned);
  rejected([&] { manager.free(aligned); });
  manager.free(other);
  require(ub_arena_live(atomic) == 0, "free did not reduce live bytes");
  // Mixed sizes and alignments must coalesce back into one full-size block.
  auto* whole = manager.malloc(128 * 1024);
  require(whole == region + 64, "freed ranges or alignment gaps were lost");
  manager.free(whole);
  void *zero1 = manager.malloc(0), *zero2 = manager.malloc(0);
  require(zero1 != zero2, "zero-byte allocations alias");
  manager.free(zero1); manager.free(zero2);
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) threads.emplace_back([&,t] {
    for (int round = 0; round < 1024; ++round) {
      const size_t bytes = 64 * (1 + (round + t) % 32);
      auto* pointer = static_cast<unsigned char*>(manager.malloc(bytes));
      std::memset(pointer, t + 1, bytes);
      std::this_thread::yield();
      for (size_t i = 0; i < bytes; ++i)
        require(pointer[i] == t + 1, "concurrent allocations overlap");
      manager.free(pointer);
    }
  });
  for (auto& thread : threads) thread.join();
  require(ub_arena_live(atomic) == 0, "concurrent free leaked blocks");
  whole = manager.malloc(128 * 1024); manager.free(whole);
  // A failed allocation must leave the arena usable.
  bool exhausted = false;
  try { manager.malloc(SIZE_MAX); } catch (const std::bad_alloc&) { exhausted = true; }
  require(exhausted, "overflow accepted");
  whole = manager.malloc(128 * 1024); manager.free(whole);
  if (!atomic) {
    ub_configure_arena(false, region + 64, 128 * 1024, false);
    auto* first = manager.malloc(64); manager.free(first);
    auto* second = manager.malloc(64);
    require(first != second, "immutable-address policy recycled a CACHE address");
  }
}

void clevel_contract(char* data, char* nc) {
  ub_configure_arena(false, data, 64 * 1024);
  ub_configure_arena(true, nc, 4 * 1024 * 1024);
  cacheable = MemoryManager(false);
  SimThreadInfo::setup_worker_machine_count(2);
  SimThreadInfo::setup_worker_nr(5);
  SimThreadInfo::setup_worker_db_count(1);
  SimThreadInfo::setup_machine_ids(0);
  SimThreadInfo::setup_worker_ids(0, 0);
  using Table = clevel_hash<uint64_t, uint64_t, std::hash<uint64_t>, std::equal_to<uint64_t>, 4>;
  auto* table = new (shared_atomic_memory().malloc(sizeof(Table))) Table;
  table->set_thread_num(5);
  // Stop the background resizer so a deliberately stale expansion request can
  // be exercised deterministically by all four writers below.
  table->run_expand_thread.store(false);
  table->expand_thread.join();
  for (uint64_t key = 1; key <= 16; ++key) table->insert({key, key + 100}, 0, key);
  const auto cache_live = ub_arena_live(false), cache_used = ub_arena_used(false);
  for (int round = 0; round < 4096; ++round) {
    table->insert({1, 999}, 0, 1); // Unpublished duplicate KV must be recycled.
    require(!table->update({999, 999}, 0).found, "missing update unexpectedly succeeded");
    auto found = table->search(1);
    require(found.found && found.value->second == 101, "recycled candidate corrupted a published KV");
  }
  require(ub_arena_live(false) == cache_live && ub_arena_used(false) <= cache_used + 64,
          "ClevelHash rejected candidates keep consuming CACHE");

  const auto nc_live = ub_arena_live(true);
  for (int round = 0; round < 32; ++round) {
    Table::level_bucket losing_candidate;
    losing_candidate.allocate(16384);
    losing_candidate.clear();
  }
  require(ub_arena_live(true) == nc_live, "ClevelHash discarded buckets keep consuming NC");

  auto* stale = table->meta.load();
  auto old_capacity = stale->first_level->capacity;
  const auto before = ub_arena_live(true);
  std::atomic<int> ready{0};
  std::vector<std::thread> writers;
  for (int t = 0; t < 4; ++t) writers.emplace_back([&,t] {
    SimThreadInfo::setup_worker_ids(0, t);
    ready.fetch_add(1);
    while (ready.load() != 4) std::this_thread::yield();
    table->expand(t, stale);
  });
  for (auto& thread : writers) thread.join();
  require(table->meta.load()->first_level->capacity == 2 * old_capacity, "stale requests expanded repeatedly");
  const size_t new_bucket_bytes = 2 * old_capacity * sizeof(Table::bucket);
  require(ub_arena_live(true) - before <= new_bucket_bytes + 1024, "losing expanders retained extra arrays");
  for (uint64_t key = 1; key <= 16; ++key) {
    auto found = table->search(key);
    require(found.found && found.value->second == key + 100, "expansion lost a key");
  }
  table->~Table(); shared_atomic_memory().free(table);
}
}
int main() {
  const size_t bytes = 4ul << 20;
  const std::string name = "ub-reuse-" + std::to_string(getpid());
  create_ub_memory(name.c_str(), bytes, "fixture", UINT32_MAX, UbMappingMode::Cacheable);
  create_ub_memory((name + "-nc").c_str(), bytes, "fixture", UINT32_MAX, UbMappingMode::Noncacheable);
  auto* data = static_cast<char*>(map_ub_memory(name.c_str(), nullptr, bytes, UbMappingMode::Cacheable));
  auto* nc = static_cast<char*>(map_ub_memory((name + "-nc").c_str(), nullptr, bytes, UbMappingMode::Noncacheable));
  allocator_contract(false, data); allocator_contract(true, nc);
  clevel_contract(data, nc);
  unmap_ub_memory(data, bytes); unmap_ub_memory(nc, bytes);
  require(ub_arena_used(false) == 0 && ub_arena_live(true) == 0, "unmap retained allocator state");
  destroy_ub_memory(name.c_str()); destroy_ub_memory((name + "-nc").c_str());
  std::cout << "PASS UB free/reuse, alignment, coalescing, concurrent lifetimes and ClevelHash candidates\n";
}
