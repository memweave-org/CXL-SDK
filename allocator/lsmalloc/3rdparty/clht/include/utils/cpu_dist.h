#pragma once

#include <numa.h>
#include <iostream>
#include <numa.h>
#include <numaif.h>
#include <set>
#include <unistd.h>
#include <vector>
#include <mutex>

// The number of CPUs in the specific NUMA node
int get_cpu_nr_numa(int node);

// The first CPU in the specific NUMA node
int get_first_cpu_of_numa_node(int node);

// Find the next available CPU, based on the value set in set_last_cpu()
int get_available_cpu_server();

// Set the pthread_attr_t to the specific CPU
int set_pthread_affinity_attr(int cpu, pthread_attr_t &attr);

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

    // 获取并存储每个节点的CPU信息
    for (int node = 0; node < max_node; ++node) {
      struct bitmask *cpumask = numa_allocate_cpumask();
      if (numa_node_to_cpus(node, cpumask) == 0) {
        for (uint32_t cpu = 0; cpu < cpumask->size; ++cpu) {
          if (numa_bitmask_isbitset(cpumask, cpu)) {
            node_cpus[node].push_back(cpu);
          }
        }
      }
      numa_free_cpumask(cpumask);
    }
  }

  int allocate_cpu(int preferred_node) {
    std::unique_lock<std::mutex> lock(mtx);
    // 从首选节点出发，寻找可用的CPU
    for (int current_node_idx = preferred_node; current_node_idx < max_node;
         ++current_node_idx) {

      if (node_cpus[current_node_idx].empty()) {
        continue; // 如果此节点没有CPU，则跳过
      }

      int num_cpus_on_this_node = node_cpus[current_node_idx].size();
      int current_search_start_offset = next_cpu_idx_in_node[current_node_idx];

      // 从记录的起始偏移量开始，在此节点上迭代所有CPU
      for (int j = 0; j < num_cpus_on_this_node; ++j) {
        int cpu_list_idx =
            (current_search_start_offset + j) % num_cpus_on_this_node;
        int cpu_to_check = node_cpus[current_node_idx][cpu_list_idx];

        if (allocated_cpus.find(cpu_to_check) == allocated_cpus.end()) {
          allocated_cpus.insert(cpu_to_check);
          // 更新此节点的下一个CPU起始搜索索引
          next_cpu_idx_in_node[current_node_idx] =
              (cpu_list_idx + 1) % num_cpus_on_this_node;
          return cpu_to_check;
        }
      }
    }

    // 如果所有CPU都已经分配，返回错误
    std::cerr << "All CPUs have been allocated." << std::endl;
    return -1;
  }
};

extern CpuAllocator cpu_allocator;