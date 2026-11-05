#include "Cache.h"

Cache::Cache(const CacheConfig &cache_config) {
  size = cache_config.cacheSize;
// #ifdef USE_CXL
//   data = (uint64_t)cacheable.malloc(size * define::GB)
// #else
  data = (uint64_t)malloc(size * define::GB);
// #endif
}