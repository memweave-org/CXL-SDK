// Split shared bump allocators: B owns CACHE allocation; both hosts allocate NC.
#include "arena.h"
#include "shm/mm.h"
#include <atomic>
#include <cerrno>
#include <stdexcept>
struct Arena { char* begin=nullptr; size_t capacity=0; std::atomic<size_t> cursor{0}; };
static Arena data_arena, nc_arena;
static uintptr_t nc_begin, nc_end;
void configure_arena(void* base,size_t bytes) { data_arena.begin=static_cast<char*>(base);data_arena.capacity=bytes; }
void configure_nc_arena(void* base,size_t bytes,bool owner) {
  nc_begin=reinterpret_cast<uintptr_t>(base); nc_end=nc_begin+bytes;
  // Disjoint per-process slices; allocator cursors stay in local DRAM.
  size_t half=(bytes/2)&~size_t(63);
  nc_arena.begin=static_cast<char*>(base)+(owner?0:half); nc_arena.capacity=half;
}
bool is_nc_address(const void* p,size_t n) {
  uintptr_t a=reinterpret_cast<uintptr_t>(p);
  return a>=nc_begin && a<nc_end && n<=nc_end-a;
}
void ub_assert_atomic_target(const void* p) {
  uintptr_t a=reinterpret_cast<uintptr_t>(p);
  uintptr_t d=reinterpret_cast<uintptr_t>(data_arena.begin);
  if (a>=d && a<d+data_arena.capacity) throw std::runtime_error("atomic target in CACHE data arena");
}
size_t arena_used() { return data_arena.cursor.load(); }
size_t nc_arena_used() { return nc_arena.cursor.load(); }
MemoryManager cacheable;
MemoryManager ub_atomic_memory;
MemoryManager::MemoryManager() : memkind_pool(nullptr), backend_(CacheableAllocatorBackend::Memkind), base(nullptr), size(0), allocator(nullptr) {}
MemoryManager::~MemoryManager() {}
int MemoryManager::posix_memalign(void** out,size_t alignment,size_t bytes) {
  if (this==&cacheable && ub_read_only_thread) throw std::runtime_error("read-only worker attempted shared allocation");
  alignment=std::max(alignment,cache_line_bytes());
  if (!alignment || (alignment&(alignment-1))) return EINVAL;
  auto& arena=this==&ub_atomic_memory?nc_arena:data_arena;
  auto& cursor=arena.cursor;
  auto capacity=arena.capacity;
  auto begin=arena.begin;
  auto old=cursor.load();
  for (;;) {
    const auto aligned=(old+alignment-1)&~(alignment-1);
    if (aligned>capacity || bytes>capacity-aligned) return ENOMEM;
    auto next=(aligned+bytes+cache_line_bytes()-1)&~(cache_line_bytes()-1);
    if (next>capacity) return ENOMEM;
    if(cursor.compare_exchange_weak(old,next)) { *out=begin+aligned; return 0; }
  }
}
void* MemoryManager::malloc(size_t bytes) { void* p; if(posix_memalign(&p,64,bytes)) throw std::bad_alloc(); return p; }
int MemoryManager::clalign(void** p,size_t bytes) { return posix_memalign(p,64,bytes); }
void MemoryManager::free(void*) {} // Includes failed CAS allocations; retain until teardown.
