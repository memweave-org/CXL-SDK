// Debug model for B (sole CACHE writer) and A (read-only CACHE replica).
// Ordinary loads/stores touch a process-specific memfd. Cold reads fill a line;
// maintenance publishes/invalidates lines explicitly. NC stays shared.
#include "shm/local_ncc.h"
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <vector>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <time.h>

namespace {
constexpr size_t line_size = 64;
std::mutex mutex;
std::atomic<uintptr_t> cache_begin{0}, cache_end{0};
struct Replica {
  char *address = nullptr, *alias = nullptr, *backing = nullptr;
  size_t bytes = 0;
  int fd = -1;
  bool writer = false;
  bool check_reads = false;
  unsigned long sequence = 0, pushed = 0, pulled = 0, skipped = 0;
  unsigned long stores = 0, prefetched = 0, invalidated = 0;
  unsigned long checked_reads = 0;
  size_t read_ahead = 0;
  // valid=false is EMPTY: replica bytes may remain zero/stale, but cannot be
  // consumed until fill() fetches the public line. A valid cached zero is a
  // different state and must not be refreshed implicitly.
  std::vector<unsigned char> valid, dirty;
  FILE* trace = nullptr;
  std::array<std::array<char, 192>, 32> recent{};
} replica;

struct BackingLock {
  BackingLock() {
    while (flock(replica.fd, LOCK_EX))
      if (errno != EINTR) throw std::runtime_error("LOCAL_NCC backing lock failed");
  }
  ~BackingLock() { flock(replica.fd, LOCK_UN); }
};
void event(size_t offset, const char* operation) {
  auto& slot = replica.recent[replica.sequence % replica.recent.size()];
  struct timespec now{}; clock_gettime(CLOCK_MONOTONIC, &now);
  const auto ns = static_cast<unsigned long long>(now.tv_sec)*1000000000ull+now.tv_nsec;
  std::snprintf(slot.data(), slot.size(),
      "LOCAL_NCC_LINE pid=%ld role=%c seq=%lu ns=%llu offset=0x%zx action=%s\n",
      long(getpid()), replica.writer ? 'B' : 'A', ++replica.sequence, ns, offset, operation);
  if (replica.trace) { std::fputs(slot.data(), replica.trace); std::fflush(replica.trace); }
}
bool drop(const char* direction) {
  const char* value = std::getenv("LOCAL_NCC_DROP");
  return value && std::strcmp(value, direction) == 0;
}
// Called under the process-local mutex. Copies from the public backing store,
// never from the other replica. EMPTY becomes valid only at this demand read,
// RFO, or explicit/simulated prefetch. Valid lines (even zero) stay cached.
void fill(size_t offset, bool prefetch = false) {
  if (replica.valid[offset/line_size]) return;
  BackingLock lock;
  std::memcpy(replica.alias+offset, replica.backing+offset, line_size);
  replica.valid[offset/line_size] = true;
  ++replica.pulled;
  if (prefetch) ++replica.prefetched;
  event(offset, prefetch ? "memory-to-cache-prefetch" : "memory-to-cache-cold-fill");
}
bool contains(const void* address, size_t bytes) {
  const auto base = cache_begin.load(std::memory_order_relaxed);
  const auto first = reinterpret_cast<uintptr_t>(address);
  if (!base || !bytes) return false;
  if (bytes > UINTPTR_MAX-first) throw std::runtime_error("LOCAL_NCC access range overflow");
  const auto end = cache_end.load(std::memory_order_relaxed);
  if (first >= end || first+bytes <= base) return false;
  if (first < base || first+bytes > end) throw std::runtime_error("LOCAL_NCC access crosses mapping boundary");
  return true;
}
void access(const void* address, size_t bytes, bool writing) {
  if (!contains(address, bytes)) return;
  std::lock_guard<std::mutex> guard(mutex);
  // Let the application's PROT_READ mapping trap an attempted A store.
  if (writing && !replica.writer) return;
  const auto first = reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(replica.address);
  for (size_t offset = first & ~(line_size-1); offset < first+bytes; offset += line_size) {
    fill(offset); // Write allocate/RFO also preserves untouched words in the line.
    if (writing) { replica.dirty[offset/line_size] = true; ++replica.stores; }
    // Bounded next-line prefetch, including lines not yet allocated/published.
    for (size_t n = 1; n <= replica.read_ahead && n < (replica.bytes-offset)/line_size; ++n)
      fill(offset+n*line_size, true);
  }
  if (!replica.writer && replica.check_reads && !writing) {
    BackingLock lock;
    ++replica.checked_reads;
    if (std::memcmp(replica.alias+first, replica.backing+first, bytes)) {
      event(first & ~(line_size-1), "stale-demand-read");
      size_t mismatch = 0;
      while (replica.alias[first+mismatch] == replica.backing[first+mismatch]) ++mismatch;
      std::fprintf(stderr, "LOCAL_NCC_STALE_READ pid=%ld role=A address=%p bytes=%zu offset=0x%zx local=0x%02x backing=0x%02x\n",
          long(getpid()), address, bytes, first+mismatch,
          static_cast<unsigned char>(replica.alias[first+mismatch]),
          static_cast<unsigned char>(replica.backing[first+mismatch]));
      std::fflush(stderr); std::abort(); // Observe only: never repair a stale load.
    }
  }
}
}

void* local_ncc_map(int backing_fd, void* address, size_t bytes, int prot, int flags) {
  std::lock_guard<std::mutex> guard(mutex);
  const char* initial = std::getenv("LOCAL_NCC_INITIAL");
  const bool stale = !initial || std::strcmp(initial, "stale") == 0;
  if (!stale && std::strcmp(initial, "cold") != 0)
    throw std::runtime_error("LOCAL_NCC_INITIAL must be cold or stale");
  size_t read_ahead = 0;
  if (const char* ahead = std::getenv("LOCAL_NCC_READ_AHEAD")) {
    char* end = nullptr; read_ahead = std::strtoul(ahead, &end, 10);
    if (!*ahead || *end || read_ahead > 64)
      throw std::runtime_error("LOCAL_NCC_READ_AHEAD must be 0..64 cachelines");
  }
  if (replica.address || bytes % line_size || !bytes ||
      (prot != PROT_READ && prot != (PROT_READ | PROT_WRITE))) {
    errno = EINVAL; return MAP_FAILED;
  }
  const int fd = dup(backing_fd);
  if (fd < 0) return MAP_FAILED;
  const bool writer = prot & PROT_WRITE;
  // A second writer is outside this model; reject it rather than simulate it incorrectly.
  struct flock owner{};
  owner.l_type = F_WRLCK; owner.l_whence = SEEK_SET; owner.l_len = 1;
  // OFD locks survive closing the SDK's other descriptor for the same file.
  if (writer && fcntl(fd, F_OFD_SETLK, &owner)) { close(fd); return MAP_FAILED; }
  const int private_fd = memfd_create("local-ncc-replica", MFD_CLOEXEC);
  if (private_fd < 0) { close(fd); return MAP_FAILED; }
  if (ftruncate(private_fd, bytes)) { close(private_fd); close(fd); return MAP_FAILED; }
  auto* backing = static_cast<char*>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  auto* alias = static_cast<char*>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, private_fd, 0));
  void* view = mmap(address, bytes, prot, flags, private_fd, 0);
  struct stat identity{}; fstat(private_fd, &identity);
  close(private_fd);
  if (backing == MAP_FAILED || alias == MAP_FAILED || view == MAP_FAILED) {
    if (backing != MAP_FAILED) munmap(backing, bytes);
    if (alias != MAP_FAILED) munmap(alias, bytes);
    if (view != MAP_FAILED) munmap(view, bytes);
    close(fd); return MAP_FAILED;
  }
  replica.address = static_cast<char*>(view); replica.alias = alias;
  replica.backing = backing; replica.bytes = bytes; replica.fd = fd; replica.writer = writer;
  replica.valid.assign(bytes/line_size, stale);
  replica.dirty.assign(bytes/line_size, false);
  replica.read_ahead = read_ahead;
  const char* checks = std::getenv("LOCAL_NCC_CHECK_READS");
  replica.check_reads = checks && std::strcmp(checks, "1") == 0;
  if (const char* directory = std::getenv("LOCAL_NCC_TRACE_DIR")) {
    const auto path = std::string(directory) + "/cachelines-" + std::to_string(getpid()) + ".log";
    replica.trace = std::fopen(path.c_str(), "w");
    if (!replica.trace) {
      munmap(view, bytes); munmap(alias, bytes); munmap(backing, bytes); close(fd);
      replica = Replica{}; return MAP_FAILED;
    }
  }
  cache_end.store(reinterpret_cast<uintptr_t>(view)+bytes);
  cache_begin.store(reinterpret_cast<uintptr_t>(view));
  std::fprintf(stderr, "LOCAL_NCC_MAP pid=%ld role=%c address=%p bytes=%zu replica_inode=%lu initial=%s read_ahead=%zu performance_valid=false\n",
      long(getpid()), writer ? 'B' : 'A', view, bytes, static_cast<unsigned long>(identity.st_ino), stale ? "stale-zero" : "cold", read_ahead);
  return view;
}

void local_ncc_unmap(void* address, size_t bytes) {
  std::lock_guard<std::mutex> guard(mutex);
  if (address != replica.address || bytes != replica.bytes)
    throw std::runtime_error("LOCAL_NCC unmap mismatch");
  std::fprintf(stderr, "LOCAL_NCC_STATS pid=%ld role=%c pushed=%lu pulled=%lu skipped=%lu stores=%lu prefetched=%lu invalidated=%lu checked_reads=%lu\n",
      long(getpid()), replica.writer ? 'B' : 'A', replica.pushed, replica.pulled, replica.skipped,
      replica.stores, replica.prefetched, replica.invalidated, replica.checked_reads);
  if (replica.trace) std::fclose(replica.trace);
  cache_begin.store(0); cache_end.store(0);
  munmap(replica.alias, bytes); munmap(replica.backing, bytes); close(replica.fd);
  replica = Replica{}; // SDK unmaps the application view separately.
}

void local_ncc_maintain(const void* address, size_t bytes, bool writeback) {
  std::lock_guard<std::mutex> guard(mutex);
  const auto base = reinterpret_cast<uintptr_t>(replica.address);
  const auto first = reinterpret_cast<uintptr_t>(address);
  if (!replica.address || first < base || first-base > replica.bytes || bytes > replica.bytes-(first-base))
    throw std::runtime_error("LOCAL_NCC cache maintenance outside replica");
  if (!bytes) return;
  // The initial whole-arena invalidate has no dirty lines. Preserve its exact
  // effect without generating millions of identical per-line trace records.
  if (!writeback && first == base && bytes == replica.bytes &&
      std::none_of(replica.dirty.begin(), replica.dirty.end(), [](unsigned char d) { return d != 0; })) {
    if (drop("invalidate")) {
      replica.skipped += bytes/line_size;
      event(0, "DROP-invalidate-whole-arena");
    } else {
      std::fill(replica.valid.begin(), replica.valid.end(), false);
      replica.invalidated += bytes/line_size;
      event(0, "invalidate-whole-arena");
    }
    return;
  }
  for (size_t offset = (first-base) & ~(line_size-1); offset < first-base+bytes; offset += line_size) {
    const auto line = offset/line_size;
    if (replica.dirty[line]) {
      if (drop("writeback")) { ++replica.skipped; event(offset, "DROP-B-to-memory"); continue; }
      BackingLock lock;
      std::memcpy(replica.backing+offset, replica.alias+offset, line_size);
      replica.dirty[line] = false;
      ++replica.pushed; event(offset, writeback ? "dirty-to-memory-clwb" : "dirty-to-memory-clflush");
    } else if (writeback) {
      event(offset, "clean-clwb-no-refill");
    }
    if (!writeback) {
      if (drop("invalidate")) { ++replica.skipped; event(offset, "DROP-invalidate"); continue; }
      replica.valid[line] = false;
      ++replica.invalidated; event(offset, "invalidate");
    }
  }
}

extern "C" void local_ncc_read(const void* address, size_t bytes) {
  access(address, bytes, false);
}
extern "C" void local_ncc_write(void* address, size_t bytes) {
  access(address, bytes, true);
}
extern "C" void local_ncc_atomic(const void* address, size_t bytes) {
  if (contains(address, bytes))
    throw std::runtime_error("LOCAL_NCC requires atomics in shared NC memory, not private CACHE replicas");
}
extern "C" void local_ncc_prefetch(const void* address) {
  // Prefetch is non-faulting for unmapped/out-of-range pointers.
  if (!contains(address, 1)) return;
  std::lock_guard<std::mutex> guard(mutex);
  const auto offset = (reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(replica.address)) & ~(line_size-1);
  fill(offset, true);
}

[[noreturn]] void local_ncc_bad_pointer(const void* node, const void* field, const char* name) {
  std::lock_guard<std::mutex> guard(mutex);
  std::fprintf(stderr, "LOCAL_NCC_BAD_POINTER pid=%ld role=%c node=%p field=%s field_address=%p local=NULL\n",
      long(getpid()), replica.writer ? 'B' : 'A', node, name, field);
  const auto base = reinterpret_cast<uintptr_t>(replica.address);
  const auto value = reinterpret_cast<uintptr_t>(field);
  if (replica.address && value >= base && value-base <= replica.bytes-sizeof(uintptr_t)) {
    BackingLock lock;
    uintptr_t published = 0;
    std::memcpy(&published, replica.backing+(value-base), sizeof(published));
    std::fprintf(stderr, "LOCAL_NCC_FIELD offset=0x%zx backing=0x%lx diagnosis=%s\n", value-base,
        static_cast<unsigned long>(published), published ? "reader-missing-invalidation" : "backing-also-null-check-publication");
    const auto offset = (value-base) & ~(line_size-1);
    for (size_t word = 0; word < line_size; word += sizeof(uintptr_t)) {
      uintptr_t local = 0, backing = 0;
      std::memcpy(&local, replica.alias+offset+word, sizeof(local));
      std::memcpy(&backing, replica.backing+offset+word, sizeof(backing));
      std::fprintf(stderr, "LOCAL_NCC_WORD offset=0x%zx local=0x%lx backing=0x%lx\n",
          offset+word, static_cast<unsigned long>(local), static_cast<unsigned long>(backing));
    }
  }
  const auto start = replica.sequence > replica.recent.size() ? replica.sequence-replica.recent.size() : 0;
  for (auto i = start; i < replica.sequence; ++i)
    std::fputs(replica.recent[i % replica.recent.size()].data(), stderr);
  std::fflush(stderr);
  std::abort();
}
