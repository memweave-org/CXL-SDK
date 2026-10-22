#include "shm/ub.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <atomic>
#include <new>
#include <limits>
#include <algorithm>
#ifdef SHM_LIB_WITH_UB
extern "C" {
#include "shm/memlink_client.h"
}
#elif defined(SHM_LIB_WITH_UBSM)
#include <ubs_mem.h>
#include <ubs_mem_def.h>
#endif

namespace {
void checked(int rc, const char* operation) {
  if (rc) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(rc));
}
std::mutex api_mutex;
std::map<void*, size_t> mappings;
std::map<void*, UbMappingMode> mapping_modes;
std::map<void*, std::string> mapping_names;
struct Arena { char* base{}; size_t size{}; std::atomic<size_t> used{0}; };
Arena arenas[2];
// Published once before starting worker threads, unchanged until they join.
std::atomic<uintptr_t> cache_begin{0}, cache_end{0};
#ifdef SHM_LIB_WITH_UBSM
bool initialized = false;
void initialize() {
  if (initialized) return;
  ubsmem_options_t options{};
  checked(ubsmem_init_attributes(&options), "ubsmem_init_attributes");
  checked(ubsmem_initialize(&options), "ubsmem_initialize");
  initialized = true;
}
void finalize() {
  if (initialized && mappings.empty()) {
    checked(ubsmem_finalize(), "ubsmem_finalize");
    initialized = false;
  }
}
void validate_name(const char* name) {
  if (!name || !*name || std::strlen(name) > 48)
    throw std::runtime_error("UBSM object name must contain 1..48 characters");
}
#endif
}

const char* ub_api_name() {
#ifdef SHM_LIB_WITH_UBSM
  return "ubsm";
#elif defined(SHM_LIB_WITH_UB)
  return "memlink";
#else
  return "none";
#endif
}

void* map_ub_memory(const char* name, void* base, size_t size, UbMappingMode mode) {
  std::lock_guard<std::mutex> guard(api_mutex);
  void* p = nullptr;
#ifdef SHM_LIB_WITH_UBSM
  validate_name(name);
  if (mode == UbMappingMode::Cacheable && cache_begin.load())
    throw std::runtime_error("Only one cacheable UB arena is supported");
  initialize();
  const int rc = ubsmem_shmem_map(base, size, PROT_READ | PROT_WRITE,
      MAP_SHARED | (base ? MAP_FIXED_NOREPLACE : 0), name, 0, &p);
  if (rc) {
    finalize();
    checked(rc, "ubsmem_shmem_map");
  }
  if (!p || p == MAP_FAILED || (base && p != base)) {
    if (p && p != MAP_FAILED) checked(ubsmem_shmem_unmap(p, size), "unmap unexpected address");
    finalize();
    throw std::runtime_error("UBSM did not map the requested fixed address");
  }
#elif defined(SHM_LIB_WITH_UB)
  int error = 0;
  p = MemlinkMemShmMmap(base, size, PROT_READ | PROT_WRITE, MAP_SHARED, name, 0, &error);
  checked(error, "MemlinkMemShmMmap");
  if (!p || p == MAP_FAILED || (base && p != base)) {
    if (p && p != MAP_FAILED) {
      MemlinkMemShmUnmmap(p, size, &error);
      checked(error, "Memlink unmap unexpected address");
    }
    throw std::runtime_error("Memlink did not map the requested fixed address");
  }
#else
  throw std::runtime_error("UB is disabled; configure global UB_API=memlink or ubsm");
#endif
#ifdef SHM_LIB_WITH_UBSM
  if (mode == UbMappingMode::Cacheable) {
    const int rc = ubsmem_shmem_set_ownership(name, p, size, PROT_READ | PROT_WRITE);
    if (rc) {
      checked(ubsmem_shmem_unmap(p, size), "unmap after ownership failure");
      finalize(); checked(rc, "ubsmem_shmem_set_ownership(RW)");
    }
    cache_end.store(uintptr_t(p) + size); cache_begin.store(uintptr_t(p));
  }
#endif
  mappings.emplace(p, size);
  mapping_modes.emplace(p, mode);
  mapping_names.emplace(p, name);
  std::cout << "UB_MAPPING api=" << ub_api_name() << " name=" << name
            << " address=" << p << " bytes=" << size << std::endl;
  return p;
}

void unmap_ub_memory(void* base, size_t size) {
  std::lock_guard<std::mutex> guard(api_mutex);
  auto it = mappings.find(base);
  if (it == mappings.end() || it->second != size)
    throw std::runtime_error("UB unmap must use the exact original address and length");
#ifdef SHM_LIB_WITH_UBSM
  if (mapping_modes.at(base) == UbMappingMode::Cacheable)
    checked(ubsmem_shmem_set_ownership(mapping_names.at(base).c_str(), base, size, PROT_NONE),
            "ubsmem_shmem_set_ownership(NONE)");
  checked(ubsmem_shmem_unmap(base, size), "ubsmem_shmem_unmap");
#elif defined(SHM_LIB_WITH_UB)
  int error = 0;
  MemlinkMemShmUnmmap(base, size, &error);
  checked(error, "MemlinkMemShmUnmmap");
#endif
  if (mapping_modes.at(base) == UbMappingMode::Cacheable) {
    cache_begin.store(0); cache_end.store(0);
  }
  for (auto& arena : arenas) {
    const auto a = uintptr_t(arena.base), b = uintptr_t(base);
    if (arena.base && a >= b && a-b < size) { arena.base=nullptr; arena.size=0; arena.used.store(0); }
  }
  mapping_modes.erase(base);
  mapping_names.erase(base);
  mappings.erase(it);
#ifdef SHM_LIB_WITH_UBSM
  finalize();
#endif
}

void create_ub_memory(const char* name, size_t size, const char* host, uint32_t numa, UbMappingMode mode) {
  std::lock_guard<std::mutex> guard(api_mutex);
#ifdef SHM_LIB_WITH_UBSM
  validate_name(name);
  ubs_mem_provider_t provider{};
  if (!host || !*host || std::strlen(host) >= sizeof(provider.host_name))
    throw std::runtime_error("A valid UBSM provider hostname is required");
  if (!size || size % (4ul << 20))
    throw std::runtime_error("UBSM size must be a positive multiple of 4 MiB");
  std::strcpy(provider.host_name, host);
  provider.socket_id = provider.port_id = UINT32_MAX;
  provider.numa_id = numa;
  initialize();
  const uint64_t flags = mode == UbMappingMode::Cacheable ? UBSM_FLAG_CACHE :
      (mode == UbMappingMode::Noncacheable ? UBSM_FLAG_NONCACHE : UBSM_FLAG_ONLY_IMPORT_NONCACHE) | UBSM_FLAG_WR_DELAY_COMP;
  const int rc = ubsmem_shmem_allocate_with_provider(&provider, name, size, 0660, flags);
  if (!rc) std::cout << "UB_OBJECT_CREATED name=" << name
                     << " requested_provider=" << host << " numa=" << numa
                     << " mode=" << int(mode) << " flags=" << flags << std::endl;
  finalize();
  checked(rc, "ubsmem_shmem_allocate_with_provider");
#else
  throw std::runtime_error("Automatic object creation requires UB_API=ubsm");
#endif
}

void destroy_ub_memory(const char* name) {
  std::lock_guard<std::mutex> guard(api_mutex);
#ifdef SHM_LIB_WITH_UBSM
  validate_name(name);
  if (!mappings.empty()) throw std::runtime_error("Cannot destroy with local UB mappings still active");
  initialize();
  const int rc = ubsmem_shmem_deallocate(name);
  finalize();
  checked(rc, "ubsmem_shmem_deallocate");
  std::cout << "UB_OBJECT_DESTROYED name=" << name << std::endl;
#else
  throw std::runtime_error("Automatic object destruction requires UB_API=ubsm");
#endif
}

void ub_configure_arena(bool atomic, void* base, size_t size) {
  std::lock_guard<std::mutex> guard(api_mutex);
  const auto address = uintptr_t(base);
  const auto expected = atomic ? UbMappingMode::Noncacheable : UbMappingMode::Cacheable;
  bool valid = false;
  for (const auto& item : mappings) {
    const auto start = uintptr_t(item.first);
    if (address >= start && address-start <= item.second && size <= item.second-(address-start)
        && mapping_modes.at(item.first) == expected) valid = true;
  }
  if (!valid || !size || address % 64) throw std::runtime_error("UB arena does not match mapping mode/range");
  auto& a = arenas[atomic]; a.base = static_cast<char*>(base); a.size = size; a.used.store(0);
}
void* ub_arena_allocate(bool atomic, size_t size, size_t alignment) {
  auto& a = arenas[atomic];
  if (!a.base || !alignment || (alignment & (alignment-1))) throw std::bad_alloc();
  alignment = std::max(alignment, size_t(64));
  auto old = a.used.load(std::memory_order_relaxed);
  for (;;) {
    if (old > a.size || alignment-1 > a.size-old) throw std::bad_alloc();
    const size_t start = (old + alignment-1) & ~(alignment-1);
    if (start > a.size || size > a.size-start || size > SIZE_MAX-63) throw std::bad_alloc();
    const size_t padded = (size + 63) & ~size_t(63);
    if (padded > a.size-start) throw std::bad_alloc();
    const size_t next = start + padded;
    if (a.used.compare_exchange_weak(old,next,std::memory_order_relaxed)) return a.base + start;
  }
}
size_t ub_arena_used(bool atomic) { return arenas[atomic].used.load(); }
void ub_assert_atomic_target(const void* p) {
  const auto v = uintptr_t(p), b = cache_begin.load(std::memory_order_relaxed);
  if (b && v >= b && v < cache_end.load(std::memory_order_relaxed))
    throw std::runtime_error("Atomic target is in cacheable UB memory");
}
void ub_cache_maintenance(const void* p, size_t n, bool writeback, bool fence) {
  const auto b = cache_begin.load(std::memory_order_relaxed), e = cache_end.load(std::memory_order_relaxed);
  const auto first = uintptr_t(p);
  if (n > UINTPTR_MAX-first) throw std::runtime_error("cache range overflow");
  const auto lo = std::max(first,b), hi = std::min(first+n,e);
  if (b && lo < hi) {
    for (auto v = lo & ~uintptr_t(63); v < hi; v += 64) {
#if defined(__aarch64__)
      if (writeback) asm volatile("dc cvac, %0" :: "r"(v) : "memory");
      else asm volatile("dc civac, %0" :: "r"(v) : "memory");
#elif defined(__x86_64__)
      asm volatile("clflush (%0)" :: "r"(v) : "memory");
#endif
    }
  }
  if (fence) {
#if defined(__aarch64__)
    asm volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
  }
}
