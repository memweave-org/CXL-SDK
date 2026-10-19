#pragma once
#include <cstdint>
#include "../../cxl_transport/memory.h"

#include "Common.h"
#include "utils/atomic_variable.h"

extern nt<uint64_t> *global_lock_table;
extern nt<void *> *root_ptr_list;

// UC builds keep remote locks in a separate WB CXL mapping accessed via nt<>.
extern int remote_lock_numa_node;
void init_global_lock_table();

inline void init_root_ptr() {
  if (root_ptr_list) return;
// #ifdef USE_CXL
//   root_ptr_list =
//       (nt<void *> *)cacheable.malloc(sizeof(void *) * define::kMaxTree);
// #else
  root_ptr_list = (nt<void *> *)cxl_transport::allocate(sizeof(nt<void *>) * define::kMaxTree);
// #endif
  for (auto i = 0UL; i < define::kMaxTree; i++)
    new (&root_ptr_list[i]) nt<void *>(nullptr);
}
