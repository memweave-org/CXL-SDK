#pragma once
#ifdef YCSB_UB_TWO_HOST
#include <cstdint>
#include <string>
#include "core/properties.h"
namespace ycsbc_ub {
void stage(const std::string& message);
void reply(const std::string& message);
int threads(int machine); // B is native master 0, A is follower 1.
int first_thread(int machine);
int cpu_start();
int worker_cpu(unsigned local_index);
bool nc_data();
bool nc_direct_data();
void prepare(utils::Properties& props);
void* control_header();
void ready();
void await_cleanup();
void finish();
}
#endif
