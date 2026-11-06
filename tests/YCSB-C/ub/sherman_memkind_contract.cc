// Bounded regression for the real memkind fixed-pool allocation footprint.
#include "../../../ds/cxl_transport/memory.h"
#include "shm/mm.h"
#include <unistd.h>
#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
  alarm(20);
  try {
    constexpr size_t capacity = 32ul << 20;
    ub_atomic_memory = MemoryManager(new SystemMemoryMmapper, nullptr, capacity);
    ub_atomic_memory.require_shared_allocations();
    cxl_transport::configure_nc_nodes(true);
    std::vector<void*> nodes;
    // 8,192 1 KiB nodes must fit comfortably in 32 MiB, including headers.
    // The old page-aligned allocation consumed about 160 MiB in memkind 1.13.
    for (size_t i = 0; i < 8192; ++i)
      nodes.push_back(cxl_transport::allocate(1024));
    memkind_update_cached_stats();
    size_t active = 0;
    if (memkind_get_stat(ub_atomic_memory.memkind_pool, MEMKIND_STAT_TYPE_ACTIVE, &active)
        || active > (24ul << 20))
      throw std::runtime_error("NC node allocation footprint exceeds budget");

    // Check independent sequences for neighboring nodes in the same OS page.
    size_t i = 0;
    while (i + 1 < nodes.size() && (uintptr_t(nodes[i]) >> 12) != (uintptr_t(nodes[i+1]) >> 12)) ++i;
    if (i + 1 == nodes.size()) throw std::runtime_error("No adjacent nodes share an OS page");
    const uint64_t value = 123456;
    cxl_transport::begin_inplace_write(nodes[i], sizeof(value));
    cxl_transport::write_plain(nodes[i+1], &value, sizeof(value));
    cxl_transport::end_inplace_write();
    uint64_t observed = 0;
    cxl_transport::read_plain(&observed, nodes[i+1], sizeof(observed));
    if (observed != value) throw std::runtime_error("Adjacent node snapshot failed");

    bool rejected = false;
    try { ub_atomic_memory.malloc(2 * capacity); }
    catch (const std::bad_alloc&) { rejected = true; }
    if (!rejected) throw std::runtime_error("Shared pool overflow escaped to heap");
    for (auto node : nodes) cxl_transport::release(node);
    memkind_update_cached_stats();
    size_t allocated = 1;
    if (memkind_get_stat(ub_atomic_memory.memkind_pool, MEMKIND_STAT_TYPE_ALLOCATED, &allocated)
        || allocated != 0)
      throw std::runtime_error("Released NC slots were not reclaimed");
    std::cout << "PASS: 8192 NC nodes in 32 MiB; active=" << active
              << "; independent adjacent sequences; strict overflow; reclaimed slots\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
