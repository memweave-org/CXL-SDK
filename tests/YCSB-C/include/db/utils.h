//
//  shm_ds_db.h
//  YCSB-C
//
//  Created by FangnuoWu on 12/26/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#ifndef YCSB_C_DB_UTILS_H_
#define YCSB_C_DB_UTILS_H_

#include <cstdint>
#include <iostream>

#include "core/db.h"

#define YCSB_KEY_LEN (16)
#define YCSB_VALUE_LEN (15)

void hex_dump(const char *filename, const void* addr, int len);

// namespace ycsb {
[[maybe_unused]] static inline uint64_t convert(const std::string &key) {
  uint64_t result = 0;
  for (size_t i = 4; i < 4 + YCSB_KEY_LEN - 1; ++i) {
    result = result * 10 + (key[i] - '0');
  }
  return result;
}

[[maybe_unused]] static inline uint64_t convert_std_hash(
    const std::string &key) {
  static auto hash_func = std::hash<std::string>{};
  return hash_func(key);
}

[[maybe_unused]] static inline void convert2(const std::string &key, uint8_t *buf) {
  for (size_t i = 0; i < YCSB_KEY_LEN - 1; ++i) {
    buf[i] = key[i + 4];
  }
}

#if defined(ENABLE_RADIX_ART_OLC_DB) || defined(ENABLE_RADIX_ART_ROWEX_DB)
#include "OptimisticLockCoupling/Tree.h"
[[maybe_unused]] static inline void loadKey(TID tid, Key &key) {
  // Store the key of the tuple into the key vector
  // Implementation is database specific
  key.setKeyLen(sizeof(tid));
  reinterpret_cast<uint64_t *>(&key[0])[0] = __builtin_bswap64(tid);
}
#endif

#endif
