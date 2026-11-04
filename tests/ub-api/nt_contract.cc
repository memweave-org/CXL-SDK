// File-backed SDK fixture correctness test. No hardware/performance claims.
#include "utils/atomic_variable.h"
#include "utils/atomic_pointer.h"
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static_assert(std::is_same<nt<uint64_t>, ub_nt<uint64_t>>::value);
static_assert(sizeof(nt<uint64_t>) == 8);
static_assert(sizeof(nt_pointer<uint64_t>) == sizeof(void*));
static_assert(sizeof(aligned_nt<uint64_t>) == 64);
static_assert(alignof(aligned_nt<uint64_t>) == 64);

template<class Function> void rejects_cache(Function function) {
  bool rejected = false;
  try { function(); } catch (const std::runtime_error&) { rejected = true; }
  assert(rejected);
}

int main() {
  const size_t bytes = 4ul << 20;
  const std::string name = "nt-contract-" + std::to_string(getpid());
  create_ub_memory(name.c_str(), bytes, "fixture", UINT32_MAX, UbMappingMode::Noncacheable);
  create_ub_memory((name + "-cache").c_str(), bytes, "fixture", UINT32_MAX, UbMappingMode::Cacheable);
  void* nc = map_ub_memory(name.c_str(), nullptr, bytes, UbMappingMode::Noncacheable);
  void* cache = map_ub_memory((name + "-cache").c_str(), nullptr, bytes, UbMappingMode::Cacheable);
  ub_configure_arena(true, nc, bytes);
  auto* value = new (ub_arena_allocate(true, sizeof(nt<uint64_t>))) nt<uint64_t>(10);
  auto* aligned = new (ub_arena_allocate(true, sizeof(aligned_nt<uint64_t>))) aligned_nt<uint64_t>(7);
  auto* pointer = new (ub_arena_allocate(true, sizeof(nt_pointer<uint64_t>))) nt_pointer<uint64_t>();
  uint64_t* raw = value->get_raw_ptr();
  assert(static_cast<void*>(raw) == static_cast<void*>(value));
  assert(*raw == 10 && value->fetch_add(2) == 10 && *raw == 12);
  assert(value->fetch_sub(3, false) == 12 && value->load() == 9);
  assert(value->fetch_xor(3) == 9 && value->load() == 10);
  uint64_t expected = 9;
  assert(!value->compare_exchange_strong(expected, 99) && expected == 10);
  assert(value->compare_exchange_strong(expected, 20, std::memory_order_release, false));
  assert(uint64_t((*value)++) == 20 && value->load() == 21);
  ++*value;
  *value = uint64_t(25);
  nt<uint64_t> local(26);
  *value = local;
  assert(value->load() == 26);
  assert(aligned->fetch_add(2) == 7 && aligned->load() == 9);
  pointer->store(raw);
  assert(pointer->load() == raw);
  assert(!pointer->compare_exchange_strong(nullptr, raw));
  assert(pointer->compare_exchange_strong(raw, nullptr) && pointer->load() == nullptr);
  pointer->store(raw, std::memory_order_release, false);
  volatile nt_pointer<uint64_t>& volatile_pointer = *pointer;
  assert(volatile_pointer != nullptr);
  nt_pointer<uint64_t[]> array(raw);
  assert(array[int64_t(0)] == 26);
  array.store(nullptr, false);
  assert(array.load() == nullptr);
  // BwTree's nt<char*> API subtracts a byte offset encoded as a pointer.
  nt<char*> tail(static_cast<char*>(nc) + 1024);
  assert(tail.fetch_sub(reinterpret_cast<char*>(64)) == static_cast<char*>(nc) + 1024);
  assert(tail.load() == static_cast<char*>(nc) + 960);

  // Loads must be actual reads: no hidden RMW or write to a shadow cache.
  assert(mprotect(nc, bytes, PROT_READ) == 0);
  assert(value->load(std::memory_order_acquire, false) == 26);
  assert(pointer->load(std::memory_order_acquire) == raw);
  value->flush(); pointer->flush();
  assert(mprotect(nc, bytes, PROT_READ | PROT_WRITE) == 0);

  // Guard before touching CACHE, even when callers pass the legacy false flag.
  auto* cached = static_cast<uint64_t*>(cache);
  assert(mprotect(cache, bytes, PROT_NONE) == 0);
  rejects_cache([&] { new (cache) nt<uint64_t>(1); });
  rejects_cache([&] { ub_atomic::load(cached, std::memory_order_relaxed); });
  rejects_cache([&] { ub_atomic::store(cached, uint64_t(1), std::memory_order_relaxed); });
  rejects_cache([&] { ub_atomic::compare_exchange(cached, expected, uint64_t(1), std::memory_order_seq_cst); });
  rejects_cache([&] { ub_atomic::fetch_add(cached, uint64_t(1)); });
  assert(mprotect(cache, bytes, PROT_READ | PROT_WRITE) == 0);

  // A child operates on the same mmap-backed word; the parent observes it.
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    uint64_t before = 26;
    bool ok = value->compare_exchange_strong(before, 40);
    ok = ok && value->fetch_add(2) == 40;
    _exit(ok ? 0 : 1);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(value->load() == 42 && *raw == 42);
  unmap_ub_memory(nc, bytes); unmap_ub_memory(cache, bytes);
  destroy_ub_memory(name.c_str()); destroy_ub_memory((name + "-cache").c_str());
  std::cout << "PASS UB NT mapped storage, atomics, readonly loads, CACHE rejection and process visibility\n";
}
