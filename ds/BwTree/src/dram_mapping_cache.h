#pragma once

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <new>
#include <numa.h>
#include <numaif.h>
#include <stdexcept>
#include <sys/mman.h>
#include <system_error>

// Keep process-local pointer caches outside the shared CXL allocator. On the
// supported tiered NUMA hosts, CPU-bearing memory nodes are host DRAM; CXL
// memory-only nodes must not be included. Bind before constructing/faulting
// the arrays, and fail rather than silently falling back to another tier.
template <typename Table, size_t Count> class DramMappingCache {
  Table *data_ = nullptr;
  static constexpr size_t bytes = sizeof(Table) * Count;

public:
  explicit DramMappingCache(bool shared_between_processes = false) {
    if (shared_between_processes)
      throw std::runtime_error("BwTree: private DRAM mapping cache requires a single-process tree");
    using Mask = std::unique_ptr<bitmask, decltype(&numa_bitmask_free)>;
    Mask nodes(numa_allocate_nodemask(), numa_bitmask_free);
    Mask cpus(numa_allocate_cpumask(), numa_bitmask_free);
    if (!nodes || !cpus) throw std::bad_alloc();
    for (int node = 0; node <= numa_max_node(); ++node) {
      numa_bitmask_clearall(cpus.get());
      if (numa_node_to_cpus(node, cpus.get()) == 0 &&
          numa_bitmask_weight(cpus.get()) != 0 &&
          numa_node_size64(node, nullptr) > 0)
        numa_bitmask_setbit(nodes.get(), node);
    }
    if (!numa_bitmask_weight(nodes.get()))
      throw std::runtime_error("BwTree: no CPU-bearing DRAM node for mapping cache");
    // Optional diagnostic override; otherwise retain the original placement.
    if (const char* setting = std::getenv("BWTREE_MAPPING_CACHE_NODE")) {
      char* end = nullptr;
      errno = 0;
      long node = std::strtol(setting, &end, 10);
      if (errno || end == setting || *end || node < 0 ||
          node > numa_max_node() || !numa_bitmask_isbitset(nodes.get(), node))
        throw std::runtime_error("BWTREE_MAPPING_CACHE_NODE must name a CPU-bearing DRAM node");
      numa_bitmask_clearall(nodes.get());
      numa_bitmask_setbit(nodes.get(), node);
    }
#ifdef BWTREE_LOCAL_NUMA
    if (!bwtree_allocation_context) throw std::logic_error("Missing mapping cache context");
    numa_bitmask_clearall(nodes.get());
    numa_bitmask_setbit(nodes.get(), bwtree_allocation_context->node);
#endif
    void *memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED)
      throw std::system_error(errno, std::generic_category(), "BwTree cache mmap");
    if (mbind(memory, bytes, MPOL_BIND, nodes->maskp, nodes->size, 0) != 0) {
      int error = errno;
      munmap(memory, bytes);
      throw std::system_error(error, std::generic_category(), "BwTree cache mbind");
    }
    data_ = static_cast<Table *>(memory);
    size_t initialized = 0;
    try {
      for (; initialized < Count; ++initialized)
        new (data_ + initialized) Table{};
    } catch (...) {
      while (initialized) data_[--initialized].~Table();
      munmap(data_, bytes);
      throw;
    }
  }
  ~DramMappingCache() {
    for (size_t i = 0; i < Count; ++i) data_[i].~Table();
    munmap(data_, bytes);
  }
  DramMappingCache(const DramMappingCache &) = delete;
  DramMappingCache &operator=(const DramMappingCache &) = delete;
  Table &operator[](size_t index) { return data_[index]; }
  const Table &operator[](size_t index) const { return data_[index]; }
};
