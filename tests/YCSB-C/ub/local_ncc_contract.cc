#include "shm/ub.h"
#include "utils/bypass_cache.h"
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <stdexcept>

constexpr size_t bytes = 4ul << 20;
void* const base = reinterpret_cast<void*>(0x300000000000ul);
void* const nc_base = reinterpret_cast<void*>(0x400000000000ul);
void send(int fd) { char c = 'x'; assert(write(fd, &c, 1) == 1); }
void receive(int fd) { char c; assert(read(fd, &c, 1) == 1); }

void visibility_contract() {
  // Isolated unit contract: all reads below are ordinary volatile loads,
  // instrumented exactly as the project's BwTree loads are.
  setenv("LOCAL_NCC_INITIAL", "cold", 1);
  setenv("SHM_UB_COW_ACQUIRE", "0", 1);
  setenv("SHM_UB_STARTUP_INVALIDATE", "0", 1);
  unsetenv("LOCAL_NCC_READ_AHEAD"); unsetenv("LOCAL_NCC_CHECK_READS");
  unsetenv("LOCAL_NCC_STARTUP_INVALIDATE"); unsetenv("LOCAL_NCC_DROP");
  struct rlimit no_core{0, 0}; setrlimit(RLIMIT_CORE, &no_core);
  const auto name = "ncc-contract-" + std::to_string(getpid());
  create_ub_memory(name.c_str(), bytes, "local", UINT32_MAX, UbMappingMode::Cacheable);
  create_ub_memory((name+"-nc").c_str(), bytes, "local", UINT32_MAX, UbMappingMode::Noncacheable);
  int to_a[2], to_b[2]; assert(pipe(to_a) == 0 && pipe(to_b) == 0);
  const auto child = fork(); assert(child >= 0);
  if (!child) {
    close(to_a[1]); close(to_b[0]);
    receive(to_a[0]); // B has mapped and owns the exclusive writer role.
    bool rejected = false;
    try { map_ub_memory(name.c_str(), base, bytes, UbMappingMode::Cacheable); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected); // A second writer must fail, even after SDK closes its fd.
    auto* data = static_cast<volatile uint64_t*>(map_ub_memory(name.c_str(), base, bytes, UbMappingMode::Cacheable, true));
    auto* nc = static_cast<volatile uint64_t*>(map_ub_memory((name+"-nc").c_str(), nc_base, bytes, UbMappingMode::Noncacheable));
    assert(data[0] == 0); // B's ordinary store must not leak through local hardware CC.
    assert(nc[0] == 17); // NC remains truly shared.
    send(to_b[1]); receive(to_a[0]);
    assert(data[0] == 0); // B writeback cannot invalidate a valid A line.
    mfence(); assert(data[0] == 0); // Fences alone do not transfer data.
    clwb(base, 8); assert(data[0] == 0); // Cleaning a clean A line does not refill it.
    clflush(base, 8); assert(data[0] == 11);
    send(to_b[1]); receive(to_a[0]);
    assert(data[0] == 11); // New B publication still cannot change the cached copy.
    assert(data[8] == 33); // Fresh CoW line: first read sees prior writeback, no invalidation.
    assert(data[16] == 0); // Adjacent untouched line was not accidentally copied.
    clflush(base, 8); assert(data[0] == 22);
    send(to_b[1]); receive(to_a[0]);
    auto* chars = reinterpret_cast<volatile unsigned char*>(data);
    assert(chars[127] == 0 && chars[128] == 0); // Both lines already valid.
    clflush(reinterpret_cast<char*>(base)+127, 2);
    assert(chars[127] == 44 && chars[128] == 55); // Unaligned cross-line maintenance.
    assert(chars[192] == 0); // Third line has never been published.
    __builtin_prefetch(static_cast<char*>(base)+256);
    clflush(static_cast<char*>(base)+320, 8); // Early flush, then prefetch before publication.
    __builtin_prefetch(static_cast<char*>(base)+320);
    assert(data[48] == 0);
    clflush(static_cast<char*>(base)+384, 64); // EMPTY; no refill until the later read.
    send(to_b[1]); receive(to_a[0]);
    assert(data[32] == 0 && data[40] == 0); // Neither prefetch nor early flush grants coherence.
    assert(data[48] == 44); // Early invalidate + B writeback + first read needs no second flush.
    clflush(static_cast<char*>(base)+256, 128);
    assert(data[32] == 42 && data[40] == 43);
    auto write_test = fork(); assert(write_test >= 0);
    if (!write_test) { data[0] = 999; _exit(10); }
    int status; assert(waitpid(write_test, &status, 0) == write_test);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    unmap_ub_memory(base, bytes); unmap_ub_memory(nc_base, bytes);
    send(to_b[1]); _exit(0);
  }
  close(to_a[0]); close(to_b[1]);
  auto* data = static_cast<volatile uint64_t*>(map_ub_memory(name.c_str(), base, bytes, UbMappingMode::Cacheable));
  auto* nc = static_cast<volatile uint64_t*>(map_ub_memory((name+"-nc").c_str(), nc_base, bytes, UbMappingMode::Noncacheable));
  data[0] = 11; nc[0] = 17;
  send(to_a[1]); receive(to_b[0]);
  clwb(base, 8);
  send(to_a[1]); receive(to_b[0]);
  data[0] = 22; data[8] = 33; data[16] = 66;
  clwb(base, 65); // Publish exactly lines 0 and 1, not line 2.
  send(to_a[1]); receive(to_b[0]);
  auto* chars = reinterpret_cast<volatile unsigned char*>(data);
  chars[127] = 44; chars[128] = 55; chars[192] = 77;
  clwb(reinterpret_cast<char*>(base)+127, 2);
  send(to_a[1]); receive(to_b[0]);
  data[32] = 42; data[40] = 43; data[48] = 44;
  clwb(static_cast<char*>(base)+256, 192);
  send(to_a[1]); receive(to_b[0]);
  int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  unmap_ub_memory(base, bytes); unmap_ub_memory(nc_base, bytes);
  destroy_ub_memory(name.c_str()); destroy_ub_memory((name+"-nc").c_str());
  close(to_a[1]); close(to_b[0]);
  std::puts("PASS: independent replicas; cold CoW fill; stale cache; missing writeback; fence; clean-only; cross-line flush; NC; A read-only; EMPTY after early invalidate fills at next read; explicit prefetch; early-flush-then-prefetch");
}

void stale_both_contract() {
  const auto name = "ncc-stale-both-" + std::to_string(getpid());
  create_ub_memory(name.c_str(), bytes, "local", UINT32_MAX, UbMappingMode::Cacheable);
  auto* data = static_cast<volatile uint64_t*>(map_ub_memory(name.c_str(), base, bytes, UbMappingMode::Cacheable));
  data[0] = 0x1234; data[1] = 0x5678; data[8] = 55;
  clwb(base, 128);
  unmap_ub_memory(base, bytes);
  setenv("LOCAL_NCC_INITIAL", "stale", 1);
  setenv("LOCAL_NCC_READ_AHEAD", "1", 1);
  int to_a[2], to_b[2]; assert(pipe(to_a) == 0 && pipe(to_b) == 0);
  const auto child = fork(); assert(child >= 0);
  if (!child) {
    close(to_a[1]); close(to_b[0]);
    data = static_cast<volatile uint64_t*>(map_ub_memory(name.c_str(), base, bytes, UbMappingMode::Cacheable, true));
    assert(data[0] == 0); // A starts with clean cached zero despite nonzero memory.
    clwb(base, 64); // A clean stale line must not overwrite public memory.
    send(to_b[1]); receive(to_a[0]);
    clflush(base, bytes);
    assert(data[0] == 0x1234 && data[1] == 0x5678); // Also prefetches line 1.
    send(to_b[1]); receive(to_a[0]);
    assert(data[0] == 0x1234 && data[1] == 0x5678);
    assert(data[8] == 55); // It was prefetched, never explicitly read before B's update.
    clflush(base, 128);
    assert(data[0] == 0x1234 && data[1] == 0x9999 && data[8] == 66);
    unmap_ub_memory(base, bytes);
    send(to_b[1]); _exit(0);
  }
  close(to_a[0]); close(to_b[1]);
  data = static_cast<volatile uint64_t*>(map_ub_memory(name.c_str(), base, bytes, UbMappingMode::Cacheable));
  assert(data[0] == 0); // B is modeled too; being the writer does not make its cache fresh.
  clwb(base, 64); // Clean B cache must not corrupt the seeded nonzero memory either.
  receive(to_b[0]);
  clflush(base, bytes);
  assert(data[0] == 0x1234 && data[1] == 0x5678);
  send(to_a[1]); receive(to_b[0]);
  clflush(base, 64);
  data[1] = 0x9999; // Partial store after invalidation: RFO must preserve word 0.
  clflush(base, 64); // Dirty writeback followed by invalidation.
  assert(data[0] == 0x1234 && data[1] == 0x9999);
  data[8] = 66; clwb(static_cast<char*>(base)+64, 64);
  send(to_a[1]); receive(to_b[0]);
  int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  unmap_ub_memory(base, bytes); destroy_ub_memory(name.c_str());
  close(to_a[1]); close(to_b[0]);
  std::puts("PASS: BOTH replicas initially cache zero; clean flush preserves memory; dirty flush; write-allocate; next-line prefetch; startup invalidation");
}

int acquisition_calls = 0;
void acquire_line(const void* p) {
  ++acquisition_calls;
  clflush(p, 64);
}

void acquisition_contract() {
  // Exercise the real UB reader helper, including stale data prefetched before
  // publication. The simulator itself must never repair a valid stale line.
  setenv("LOCAL_NCC_INITIAL", "stale", 1);
  setenv("SHM_UB_COW_ACQUIRE", "1", 1);
  unsetenv("LOCAL_NCC_READ_AHEAD");
  const auto name = "ncc-acquire-" + std::to_string(getpid());
  create_ub_memory(name.c_str(), bytes, "local", UINT32_MAX, UbMappingMode::Cacheable);
  int to_a[2], to_b[2]; assert(pipe(to_a) == 0 && pipe(to_b) == 0);
  const auto child = fork(); assert(child >= 0);
  if (!child) {
    close(to_a[1]); close(to_b[0]);
    auto* data = static_cast<volatile uint64_t*>(map_ub_memory(name.c_str(), base, bytes, UbMappingMode::Cacheable, true));
    clflush(base, bytes); // Startup invalidation cannot prevent later prefetch.
    __builtin_prefetch(base);
    assert(data[8] == 0);
    send(to_b[1]); receive(to_a[0]);
    assert(data[0] == 0 && data[8] == 0);
    ub_acquire_immutable(base, acquire_line);
    assert(data[0] == 71 && acquisition_calls == 1);
    ub_acquire_immutable(base, acquire_line);
    assert(data[0] == 71 && acquisition_calls == 1); // Exactly once per object.
    ub_acquire_immutable_bytes(static_cast<char*>(base)+64, 128);
    assert(data[8] == 72 && data[16] == 73); // Full multi-line payload.
    auto* retry = static_cast<char*>(base)+256;
    bool failed = false;
    try { ub_acquire_immutable(retry, [](const void*) { throw std::runtime_error("retry"); }); }
    catch (const std::runtime_error&) { failed = true; }
    assert(failed);
    ub_acquire_immutable(retry, acquire_line);
    assert(data[32] == 74 && acquisition_calls == 2);
    uint64_t local = 0;
    ub_acquire_immutable(&local, acquire_line);
    assert(acquisition_calls == 2); // Local addresses bypass acquisition.
    unmap_ub_memory(base, bytes);
    data = static_cast<volatile uint64_t*>(map_ub_memory(name.c_str(), base, bytes, UbMappingMode::Cacheable, true));
    assert(data[0] == 0); // Remapping resets both replica and acquired registry.
    ub_acquire_immutable(base, acquire_line);
    assert(data[0] == 71 && acquisition_calls == 3);
    unmap_ub_memory(base, bytes);
    send(to_b[1]); _exit(0);
  }
  close(to_a[0]); close(to_b[1]);
  auto* data = static_cast<volatile uint64_t*>(map_ub_memory(name.c_str(), base, bytes, UbMappingMode::Cacheable));
  receive(to_b[0]);
  data[0] = 71; data[8] = 72; data[16] = 73; data[32] = 74;
  ub_acquire_immutable(base, acquire_line);
  assert(acquisition_calls == 0); // Writer construction must never acquire.
  clwb(base, 320);
  send(to_a[1]); receive(to_b[0]);
  int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  unmap_ub_memory(base, bytes); destroy_ub_memory(name.c_str());
  close(to_a[1]); close(to_b[0]);
  std::puts("PASS: post-publication CoW acquire; once per object; multi-line value; failed acquire retry; writer/local bypass; remap resets registry");
}

void startup_default_contract() {
  setenv("LOCAL_NCC_INITIAL", "cold", 1);
  setenv("SHM_UB_STARTUP_INVALIDATE", "0", 1);
  setenv("SHM_UB_COW_ACQUIRE", "0", 1);
  const auto name = "ncc-startup-default-" + std::to_string(getpid());
  create_ub_memory(name.c_str(), bytes, "local", UINT32_MAX, UbMappingMode::Cacheable);
  auto* data = static_cast<volatile uint64_t*>(map_ub_memory(name.c_str(), base, bytes, UbMappingMode::Cacheable));
  data[0] = 0x1234; data[8] = 0x5678;
  clwb(base, 128);
  unmap_ub_memory(base, bytes);
  // Exercise the production default, with no runner-provided enable flag.
  // Both fresh mappings otherwise begin with valid zero-filled cache lines.
  unsetenv("SHM_UB_STARTUP_INVALIDATE");
  unsetenv("LOCAL_NCC_STARTUP_INVALIDATE");
  setenv("LOCAL_NCC_INITIAL", "stale", 1);
  for (bool readonly : {false, true}) {
    data = static_cast<volatile uint64_t*>(map_ub_memory(name.c_str(), base, bytes, UbMappingMode::Cacheable, readonly));
    assert(data[0] == 0x1234 && data[8] == 0x5678);
    unmap_ub_memory(base, bytes);
  }
  destroy_ub_memory(name.c_str());
  std::puts("PASS: production startup default invalidates initial stale-zero lines on BOTH writer and reader");
}

int main() {
  visibility_contract();
  stale_both_contract();
  acquisition_contract();
  startup_default_contract();
}
