// Tiny topology regression, no benchmark or large allocation. Override only
// topology queries so memoryless nodes can be tested on ordinary NUMA hosts.
#include "core/ub_numa.h"
#include <array>
#include <iostream>

namespace {
struct Node { bool allowed = false; long long bytes = 0; int distance = 0; };
std::array<Node, 128> topology;
void require(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}
}

extern "C" bitmask* numa_get_mems_allowed() {
  auto* mask = numa_bitmask_alloc(topology.size());
  if (!mask) return nullptr;
  numa_bitmask_clearall(mask);
  for (unsigned i = 0; i < topology.size(); ++i)
    if (topology[i].allowed) numa_bitmask_setbit(mask, i);
  return mask;
}
extern "C" long long numa_node_size64(int node, long long* free_bytes) {
  if (free_bytes) *free_bytes = 0;
  return topology.at(node).bytes;
}
extern "C" int numa_distance(int, int node) {
  return topology.at(node).distance;
}

int main() {
  using ycsbc_ub::local_memory_node;
  topology[0] = {true, 4096, 30};
  topology[1] = {true, 0, 10}; // CPU is here, but there is no memory.
  topology[2] = {true, 4096, 20};
  require(local_memory_node(1) == 2, "memoryless CPU did not use nearest memory");
  require(local_memory_node(0) == 0, "usable CPU node must retain priority");
  topology[2].allowed = false;
  require(local_memory_node(1) == 0, "fallback ignored cpuset restriction");
  require(local_memory_node(2) == 0, "disallowed CPU node was selected");
  topology[0].bytes = -1;
  topology[70] = {true, 4096, 40};
  require(local_memory_node(1) == 70, "offline nodes or node IDs above 63 mishandled");
  require(local_memory_node(-1) == 70, "unknown CPU needs a usable fallback");
  topology[0] = {true, 4096, 0};
  require(local_memory_node(1) == 70, "unknown distance outranked known distance");
  topology[70].distance = 0;
  require(local_memory_node(1) == 0, "missing distances prevented fallback");
  topology = {};
  bool rejected = false;
  try { local_memory_node(1); } catch (const std::runtime_error&) { rejected = true; }
  require(rejected, "empty allowed memory set must fail clearly");
  topology[1].allowed = true;
  rejected = false;
  try { local_memory_node(1); } catch (const std::runtime_error&) { rejected = true; }
  require(rejected, "memoryless-only allowed set must fail clearly");
  std::cout << "NUMA memory-node fallback contract passed\n";
}
