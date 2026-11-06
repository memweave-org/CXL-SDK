#pragma once
#include <cstddef>

// Link-time memkind wrappers, linked ONLY into contract executables. Production
// shm-lib has no counters, allocation registry, usable-size queries or hooks.
namespace allocator_test {
// Sum live requested sizes rounded to cache lines, for the existing GC object
// accounting assertions. This is not memkind's physical/usable memory usage.
size_t live_bytes(bool atomic);
size_t allocation_count(bool atomic);
}
