//
//  scrambled_zipfian_generator.h
//  YCSB-C
//
//  Created by Jinglei Ren on 12/8/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#ifndef YCSB_C_SCRAMBLED_ZIPFIAN_GENERATOR_H_
#define YCSB_C_SCRAMBLED_ZIPFIAN_GENERATOR_H_

#include <atomic>
#include <cstdint>
#include <random>

#include "generator.h"
#include "utils.h"
#include "zipfian_generator.h"

namespace ycsbc {

class ScrambledZipfianGenerator : public Generator<uint64_t> {
 public:
  ScrambledZipfianGenerator(
      uint64_t min, uint64_t max,
      double zipfian_const = ZipfianGenerator::kZipfianConst)
      : base_(min),
        num_items_(max - min + 1),
        generator_(min, max, zipfian_const) {
    std::random_device rd;
#ifdef YCSB_UB_TWO_HOST
    std::mt19937 gen(std::getenv("YCSB_UB_SEED") ? std::stoull(std::getenv("YCSB_UB_SEED")) : rd());
#else
    std::mt19937 gen(rd());
#endif
    std::uniform_int_distribution<uint32_t> dis;
    scramble_random_value = dis(gen);
  }

  ScrambledZipfianGenerator(uint64_t num_items)
      : ScrambledZipfianGenerator(0, num_items - 1) {}

  uint64_t Next();
  uint64_t Last();

 private:
  const uint64_t base_;
  const uint64_t num_items_;
  ZipfianGenerator generator_;

  uint32_t scramble_random_value;

  uint64_t Scramble(uint64_t value) const;
};

inline uint64_t ScrambledZipfianGenerator::Scramble(uint64_t value) const {
  return base_ + (utils::FNVHash64(value) * scramble_random_value) % num_items_;
}

inline uint64_t ScrambledZipfianGenerator::Next() {
  return Scramble(generator_.Next());
}

inline uint64_t ScrambledZipfianGenerator::Last() {
  return Scramble(generator_.Last());
}

}  // namespace ycsbc

#endif  // YCSB_C_SCRAMBLED_ZIPFIAN_GENERATOR_H_
