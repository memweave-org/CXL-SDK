//
//  utils.h
//  YCSB-C
//
//  Created by Jinglei Ren on 12/5/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#ifndef YCSB_C_UTILS_H_
#define YCSB_C_UTILS_H_

#include <algorithm>
#include <cstdint>
#include <exception>
#include <random>
#include <thread>
#include <chrono>

namespace utils {

const uint64_t kFNVOffsetBasis64 = 0xCBF29CE484222325;
const uint64_t kFNVPrime64 = 1099511628211;

inline uint64_t FNVHash64(uint64_t val) {
  uint64_t hash = kFNVOffsetBasis64;

  for (int i = 0; i < 8; i++) {
    uint64_t octet = val & 0x00ff;
    val = val >> 8;

    hash = hash ^ octet;
    hash = hash * kFNVPrime64;
  }
  return hash;
}

inline uint64_t Hash(uint64_t val) { return FNVHash64(val); }

inline double RandomDouble() {
  thread_local static std::random_device rd;
#ifdef YCSB_UB_TWO_HOST
  thread_local static std::default_random_engine generator(std::getenv("YCSB_UB_SEED") ? std::stoull(std::getenv("YCSB_UB_SEED")) : rd());
#else
  thread_local static std::default_random_engine generator(rd());
#endif
  thread_local static std::uniform_real_distribution<double> uniform(0, 1);
  return uniform(generator);
}

inline uint64_t Random64() {
  thread_local static std::random_device rd;
  thread_local static std::default_random_engine generator(rd());
  thread_local static std::uniform_int_distribution<uint64_t> distribution(
      0, UINT64_MAX);
  return distribution(generator);
}

///
/// Returns an ASCII code that can be printed to desplay
///
inline char RandomPrintChar() {
  thread_local static std::random_device rd;
  thread_local static std::mt19937 gen(rd());
  thread_local static std::uniform_int_distribution<> dis(33, 126);
  return dis(gen);
}

class Exception : public std::exception {
 public:
  Exception(const std::string &message) : message_(message) { }
  const char* what() const noexcept {
    return message_.c_str();
  }
 private:
  std::string message_;
};

inline bool StrToBool(std::string str) {
  std::transform(str.begin(), str.end(), str.begin(), ::tolower);
  if (str == "true" || str == "1") {
    return true;
  } else if (str == "false" || str == "0") {
    return false;
  } else {
    throw Exception("Invalid bool string: " + str);
  }
}

inline std::string Trim(const std::string &str) {
  auto front = std::find_if_not(str.begin(), str.end(), [](int c){ return std::isspace(c); });
  return std::string(front, std::find_if_not(str.rbegin(), std::string::const_reverse_iterator(front),
      [](int c){ return std::isspace(c); }).base());
}

inline size_t RandomThreadNum() {
  thread_local static std::random_device rd;
  thread_local static std::mt19937 gen(rd());
  thread_local static std::uniform_int_distribution<> dis(1, 65536);
  return dis(gen);
}

inline int RandomValueNum() {
  thread_local static std::random_device rd;
  thread_local static auto seed =
      std::chrono::system_clock::now().time_since_epoch().count() +
      std::hash<std::thread::id>()(std::this_thread::get_id());
  thread_local static std::mt19937 gen(seed);
  thread_local static std::uniform_int_distribution<> dis(1, INT32_MAX);
  return dis(gen);
}

void AccessValueByAddress(uintptr_t value);

} // utils

#endif // YCSB_C_UTILS_H_
