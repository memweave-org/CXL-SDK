#pragma once

// (STRING / INT)
// #define INT_YCSBC_KEY
#include "core/utils.h"
#include <cstdint>
#include <iostream>
#include <string>
#define INT_KEY_ADDR
// #define YCSB_KEY

// #define USE_MSG_QUEUE

#if defined(USE_MSG_QUEUE) && !defined(YCSB_UB_TWO_HOST)
  #define USE_MWAIT
  #define RETURN_SYNC
  // #define USE_NO_CC_QUEUE
  #define ASYNC_CLIENT
#endif

#ifdef INT_KEY_ADDR
#define VALUE_ADDR_SIZE_DEFAULT 8
extern uint32_t VALUE_ADDR_SIZE;
#endif

inline void set_value_size(uint32_t size) {
  if (size == 0) {
    std::cerr << "Value size undefined." << ' ' << "Using default value size "
              << VALUE_ADDR_SIZE_DEFAULT << "." << std::endl;
    VALUE_ADDR_SIZE = VALUE_ADDR_SIZE_DEFAULT;
    return;
  }
  VALUE_ADDR_SIZE = size;
}
