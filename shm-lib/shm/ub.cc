#include "shm/mm.h"
#include <sys/mman.h>
#include <stdexcept>
#ifdef SHM_LIB_WITH_UB
extern "C" {
#include "shm/memlink_client.h"
}
#endif
void *map_ub_memory(const char *name, void *base, size_t size) {
#ifndef SHM_LIB_WITH_UB
  throw std::runtime_error("UB requires -DWITH_UB=ON and the vendor Memlink library");
#else
  int error = 0;
  void *p = MemlinkMemShmMmap(base, size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, name, 0, &error);
  if (error || !p || p == MAP_FAILED || (base && p != base))
    throw std::runtime_error("Memlink UB mapping failed or returned a different address");
  return p;
#endif
}
void unmap_ub_memory(void *base, size_t size) {
#ifdef SHM_LIB_WITH_UB
  int error = 0;
  MemlinkMemShmUnmmap(base, size, &error);
#endif
}
