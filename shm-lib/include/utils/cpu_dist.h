#pragma once

#include <numa.h>
#include <iostream>
#include <numaif.h>
#include <set>
#include <unistd.h>
#include <sched.h>
#include <vector>
#include <mutex>

// The number of CPUs in the specific NUMA node
int get_cpu_nr_numa(int node);

// The first CPU in the specific NUMA node
int get_first_cpu_of_numa_node(int node);

// Find the next available CPU, based on the value set in set_last_cpu()
int get_available_cpu_server();

// Set the pthread_attr_t to the specific CPU
int set_pthread_affinity_attr(int cpu, pthread_attr_t *attr);

// Set current thread cpu affinity to the specific CPU
int set_pthread_affinity(int cpu);

class CpuAllocator {
private:
  std::vector<std::vector<int>> node_cpus; // 用 vector 代替 set
  std::set<int> allocated_cpus;
  std::vector<int>
      next_cpu_idx_in_node; // 为每个节点存储下一个CPU的起始搜索索引
  // System specific, equals to the NUMA node count that has CPUs
  const int max_node = 4;
  std::mutex mtx;

public:
  CpuAllocator() {
    if (numa_available() == -1) {
      std::cerr << "NUMA is not available on this system." << std::endl;
      exit(1);
    }

    node_cpus.resize(max_node);
    next_cpu_idx_in_node.resize(max_node,
                                0); // 初始化所有节点的下一个CPU索引为0

    cpu_set_t allowed_cpus;
    CPU_ZERO(&allowed_cpus);
    const bool has_affinity =
        sched_getaffinity(0, sizeof(allowed_cpus), &allowed_cpus) == 0;

    // 获取并存储每个节点的CPU信息
    for (int node = 0; node < max_node; ++node) {
      struct bitmask *cpumask = numa_allocate_cpumask();
      if (numa_node_to_cpus(node, cpumask) == 0) {
        for (uint32_t cpu = 0; cpu < cpumask->size; ++cpu) {
          if (numa_bitmask_isbitset(cpumask, cpu) &&
              (!has_affinity ||
               (cpu < CPU_SETSIZE && CPU_ISSET(cpu, &allowed_cpus)))) {
            node_cpus[node].push_back(cpu);
          }
        }
      }
      numa_free_cpumask(cpumask);
    }
  }

  int allocate_cpu(int preferred_node) {
    std::unique_lock<std::mutex> lock(mtx);
    const int first =
        (preferred_node >= 0 && preferred_node < max_node) ? preferred_node : 0;

    // Prefer the requested NUMA node, then try every other available node.
    for (int step = 0; step < max_node; ++step) {
      const int node = (first + step) % max_node;
      const int count = node_cpus[node].size();
      if (count == 0) {
        continue;
      }

      const int start = next_cpu_idx_in_node[node];
      for (int offset = 0; offset < count; ++offset) {
        const int idx = (start + offset) % count;
        const int cpu = node_cpus[node][idx];
        if (allocated_cpus.insert(cpu).second) {
          next_cpu_idx_in_node[node] = (idx + 1) % count;
          return cpu;
        }
      }
    }

    std::cerr << "All CPUs have been allocated." << std::endl;
    return -1;
  }
};

extern CpuAllocator cpu_allocator;