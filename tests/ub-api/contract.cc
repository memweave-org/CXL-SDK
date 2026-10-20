#include "shm/ub.h"
#include <sys/mman.h>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
int main() {
  constexpr size_t size=4ul<<20;
  const char* name="contract";
  auto* base=reinterpret_cast<void*>(0x400000000000);
  create_ub_memory(name,size,"test-owner",0);
  // Existing names must never be silently reused or destroyed.
  bool rejected=false;
  try {create_ub_memory(name,size,"test-owner",0);} catch(const std::exception&){rejected=true;}
  assert(rejected);
  setenv("UBSM_TEST_WRONG_ADDRESS","1",1);
  rejected=false;
  try {map_ub_memory(name,base,size);} catch(const std::exception&){rejected=true;}
  assert(rejected);
  unsetenv("UBSM_TEST_WRONG_ADDRESS");
  void* p=map_ub_memory(name,base,size);
  assert(p==base);
  rejected=false;
  try {unmap_ub_memory(p,size/2);} catch(const std::exception&){rejected=true;}
  assert(rejected);
  unmap_ub_memory(p,size);
  destroy_ub_memory(name);
  std::cout<<"PASS: fixed address, collision rejection, whole-object unmap, finalize and deallocate\n";
}
