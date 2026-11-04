// Benchmark-only shared bump allocator. B allocates; nothing is reclaimed.
#include "arena.h"
#include "shm/mm.h"
#include <atomic>
#include <cerrno>
#include <stdexcept>
static char* begin;
static size_t capacity;
static std::atomic<size_t> cursor{0};
void configure_arena(void* base,size_t bytes) { begin=static_cast<char*>(base);capacity=bytes;cursor=0; }
size_t arena_used() { return cursor.load(); }
MemoryManager cacheable;
MemoryManager::MemoryManager() : memkind_pool(nullptr), backend_(CacheableAllocatorBackend::Memkind), base(nullptr), size(0), allocator(nullptr) {}
MemoryManager::~MemoryManager() {}
int MemoryManager::posix_memalign(void** out,size_t alignment,size_t bytes) {
  if (ub_read_only_thread) throw std::runtime_error("read-only worker attempted shared allocation");
  alignment=std::max(alignment,cache_line_bytes());
  if (!alignment || (alignment&(alignment-1))) return EINVAL;
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
