#include "GlobalLock.h"
#include <sys/mman.h>
#include <numaif.h>
#include <unistd.h>
#include <stdexcept>
#include <new>

nt<uint64_t> *global_lock_table;
nt<void *> *root_ptr_list;
int remote_lock_numa_node = -1;

void init_global_lock_table() {
  if (global_lock_table) return;
  const size_t bytes = define::kNumOfLock * sizeof(nt<uint64_t>);
#ifdef SHERMAN_UC
  if (remote_lock_numa_node < 0 || remote_lock_numa_node >= 64)
    throw std::runtime_error("Remote lock NUMA node not configured");
  const size_t page = size_t(sysconf(_SC_PAGESIZE));
  const size_t mapped_bytes = (bytes + page - 1) & ~(page - 1);
  void* mapping = mmap(nullptr, mapped_bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) throw std::bad_alloc();
  unsigned long mask = 1ul << remote_lock_numa_node;
  if (mbind(mapping, mapped_bytes, MPOL_BIND, &mask, 64, 0)) {
    munmap(mapping, mapped_bytes);
    throw std::runtime_error("Cannot bind WB remote lock table to CXL node");
  }
  // Process-lifetime mapping, matching the existing tree/lock lifetime.
  global_lock_table = static_cast<nt<uint64_t>*>(mapping);
#else
  global_lock_table = static_cast<nt<uint64_t>*>(cxl_transport::allocate(bytes));
#endif
  for (size_t i = 0; i < define::kNumOfLock; ++i)
    new (&global_lock_table[i]) nt<uint64_t>(0);
}
