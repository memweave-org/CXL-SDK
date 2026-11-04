#pragma once
#ifdef YCSB_UB_TWO_HOST
#include <cstdint>
#include "core/properties.h"
namespace ycsbc_ub {
int threads(int machine); // B is native master 0, A is follower 1.
int first_thread(int machine);
int cpu_start();
void prepare(utils::Properties& props);
void* control_header();
void ready();
void await_cleanup();
void finish();
}
#endif
