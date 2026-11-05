#pragma once

#include <climits>
#include <memory>
#include <new>
#include <numa.h>
#include <sched.h>
#include <stdexcept>

namespace ycsbc_ub {
// CPU-only NUMA nodes cannot be passed to MPOL_BIND. Prefer the CPU's node,
// then the nearest memory-bearing node allowed by this thread's cpuset.
// Check total capacity, not free memory: temporary pressure is not a topology
// change. Memory-only nodes are valid fallbacks on hosts with split topology.
inline int local_memory_node(int cpu_node) {
  using Mask = std::unique_ptr<bitmask, decltype(&numa_bitmask_free)>;
  Mask allowed(numa_get_mems_allowed(), numa_bitmask_free);
  if (!allowed) throw std::bad_alloc();
  auto usable = [&](int node) {
    return node >= 0 && static_cast<unsigned>(node) < allowed->size &&
           numa_bitmask_isbitset(allowed.get(), node) &&
           numa_node_size64(node, nullptr) > 0;
  };
  if (usable(cpu_node)) return cpu_node;
  int selected = -1, nearest = INT_MAX;
  for (unsigned node = 0; node < allowed->size; ++node) {
    if (!usable(node)) continue;
    const int distance = cpu_node >= 0 ? numa_distance(cpu_node, node) : 0;
    const int rank = distance > 0 ? distance : INT_MAX;
    if (selected < 0 || rank < nearest) {
      selected = node;
      nearest = rank;
    }
  }
  if (selected < 0)
    throw std::runtime_error("No allowed NUMA node with memory for local allocation");
  return selected;
}

inline int local_memory_node() {
  const int cpu = sched_getcpu();
  return local_memory_node(cpu < 0 ? -1 : numa_node_of_cpu(cpu));
}
}
