#pragma once

#include "Config.h"

class Cache {

public:
  Cache(const CacheConfig &cache_config);

  uint64_t data;
  uint64_t size;

private:
};