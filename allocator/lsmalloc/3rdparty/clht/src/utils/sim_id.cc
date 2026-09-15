#include "utils/sim_id.h"

thread_local SimThreadInfo::THREAD_ROLE SimThreadInfo::thread_role;
SimThreadInfo::PROCESS_ROLE SimThreadInfo::process_role;
thread_local uint32_t SimThreadInfo::worker_machine_id;
thread_local uint32_t SimThreadInfo::worker_db_id;
uint32_t SimThreadInfo::worker_db_count;
std::atomic<uint32_t> SimThreadInfo::worker_db_id_allocator;
thread_local uint32_t SimThreadInfo::worker_thread_id;
uint32_t SimThreadInfo::worker_thread_count;
// std::atomic<uint32_t> SimThreadInfo::worker_thread_allocator;
thread_local uint32_t SimThreadInfo::dispatcher_thread_id;
uint32_t SimThreadInfo::worker_machine_count;
uint32_t SimThreadInfo::dispatcher_thread_count;
