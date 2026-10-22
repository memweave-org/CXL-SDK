#include "shm/ub.h"
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
int main() {
  const size_t bytes=4ul<<20;
  const std::string name="split-contract-"+std::to_string(getpid());
  create_ub_memory(name.c_str(),bytes,"fixture",UINT32_MAX,UbMappingMode::Cacheable);
  create_ub_memory((name+"-nc").c_str(),bytes,"fixture",UINT32_MAX,UbMappingMode::Noncacheable);
  auto* data=map_ub_memory(name.c_str(),nullptr,bytes,UbMappingMode::Cacheable);
  auto* nc=map_ub_memory((name+"-nc").c_str(),nullptr,bytes,UbMappingMode::Noncacheable);
  bool wrong_mode=false;
  try { ub_configure_arena(true,data,bytes); } catch(const std::runtime_error&) { wrong_mode=true; }
  assert(wrong_mode);
  ub_configure_arena(false,data,bytes); ub_configure_arena(true,nc,bytes);
  auto* value=static_cast<uint64_t*>(ub_arena_allocate(false,8));
  auto* counter=new (ub_arena_allocate(true,sizeof(std::atomic<uint64_t>))) std::atomic<uint64_t>(0);
  assert(uintptr_t(value)%64==0 && uintptr_t(counter)%64==0);
  *value=42; ub_cache_maintenance(value,8,true,true);
  ub_assert_atomic_target(counter); assert(counter->fetch_add(1)==0);
  bool rejected=false;
  try { ub_assert_atomic_target(value); } catch(const std::runtime_error&) { rejected=true; }
  assert(rejected);
  // Cache maintenance must skip NC pages, even if inaccessible to the CPU.
  assert(mprotect(nc,bytes,PROT_NONE)==0);
  ub_cache_maintenance(nc,bytes,false,true);
  assert(mprotect(nc,bytes,PROT_READ|PROT_WRITE)==0);
  assert(*value==42 && counter->load()==1);
  bool exhausted=false;
  try { ub_arena_allocate(true,bytes); } catch(const std::bad_alloc&) { exhausted=true; }
  assert(exhausted);
  unmap_ub_memory(data,bytes); unmap_ub_memory(nc,bytes);
  destroy_ub_memory(name.c_str()); destroy_ub_memory((name+"-nc").c_str());
  std::cout << "PASS split placement, atomic guard, NC flush skip, bounds and lifecycle\n";
}
