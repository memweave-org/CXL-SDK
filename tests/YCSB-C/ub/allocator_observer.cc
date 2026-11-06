#include "allocator_observer.h"
#include "shm/ub.h"
#include <memkind.h>
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace {
struct Allocation { memkind_t kind; size_t bytes; bool atomic; };
struct Observer {
  std::mutex mutex;
  std::unordered_map<void*, Allocation> live;
  size_t allocations[2]{};
};
Observer& observer() {
  // Keep this test registry alive through global MemoryManager destruction.
  static auto* state = new Observer;
  return *state;
}
void record(memkind_t kind, void* pointer, size_t bytes) {
  if (!pointer) return;
  const bool atomic = ub_arena_owns(true, pointer);
  if (!atomic && !ub_arena_owns(false, pointer)) return;
  auto& state = observer();
  std::lock_guard<std::mutex> guard(state.mutex);
  state.live.emplace(pointer, Allocation{kind, (std::max(bytes, size_t(1)) + 63) & ~size_t(63), atomic});
  ++state.allocations[atomic];
}
}
extern "C" {
void* __real_memkind_malloc(memkind_t, size_t);
int __real_memkind_posix_memalign(memkind_t, void**, size_t, size_t);
void __real_memkind_free(memkind_t, void*);
int __real_memkind_destroy_kind(memkind_t);
void* __wrap_memkind_malloc(memkind_t kind, size_t bytes) {
  auto* pointer = __real_memkind_malloc(kind, bytes);
  record(kind, pointer, bytes);
  return pointer;
}
int __wrap_memkind_posix_memalign(memkind_t kind, void** pointer, size_t alignment, size_t bytes) {
  const int rc = __real_memkind_posix_memalign(kind, pointer, alignment, bytes);
  if (!rc) record(kind, *pointer, bytes);
  return rc;
}
void __wrap_memkind_free(memkind_t kind, void* pointer) {
  auto& state = observer();
  std::lock_guard<std::mutex> guard(state.mutex);
  __real_memkind_free(kind, pointer);
  state.live.erase(pointer);
}
int __wrap_memkind_destroy_kind(memkind_t kind) {
  auto& state = observer();
  std::lock_guard<std::mutex> guard(state.mutex);
  const int rc = __real_memkind_destroy_kind(kind);
  if (!rc) {
    for (auto it = state.live.begin(); it != state.live.end();) {
      if (it->second.kind == kind) it = state.live.erase(it);
      else ++it;
    }
  }
  return rc;
}
}
size_t allocator_test::live_bytes(bool atomic) {
  auto& state = observer();
  std::lock_guard<std::mutex> guard(state.mutex);
  size_t bytes = 0;
  for (const auto& entry : state.live)
    if (entry.second.atomic == atomic) bytes += entry.second.bytes;
  return bytes;
}
size_t allocator_test::allocation_count(bool atomic) {
  auto& state = observer();
  std::lock_guard<std::mutex> guard(state.mutex);
  return state.allocations[atomic];
}
