#include "utils/atomic_queued_variable.h"
#include <cstdlib>
#include <cstring>
// #include <algorithm>
// #include <array>
// #include <atomic>
// #include <cstdint>
// #include <ctime>    // For clock_gettime, CLOCK_MONOTONIC, timespec
// #include <unistd.h> // Included in original file

// // Definition of the static member variable
std::array<std::atomic<AtomicRequestQueue::time_point>,
           AtomicRequestQueue::bucket_count>
    AtomicRequestQueue::req_queue;
std::array<AtomicRequestQueue::IsolatedBucket, AtomicRequestQueue::bucket_count>
    AtomicRequestQueue::isolated_req_queue;
const bool AtomicRequestQueue::isolated_buckets = [] {
  const char* mode=std::getenv("PCC_ISOLATE_TIMING_BUCKETS");
  return mode && std::strcmp(mode,"1")==0;
}();
uint64_t AtomicRequestQueue::blocked_cnt = 0;

// // Definitions of static member functions

// inline AtomicRequestQueue::time_point AtomicRequestQueue::get_time() {
//   struct timespec ts;
//   clock_gettime(CLOCK_MONOTONIC, &ts);
//   return static_cast<time_point>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
// }

// inline void AtomicRequestQueue::wait_until(time_point until_time) {
//   while (get_time() < until_time)
//     ; // Busy wait
// }

// inline uint32_t AtomicRequestQueue::bucket_hash(const void *addr) {
//   return (reinterpret_cast<uintptr_t>(addr) >> CACHE_LINE_SHIFT) &
//          bucket_hash_mask;
// }

// template <AtomicRequestQueue::time_point ProcessTime>
// inline void AtomicRequestQueue::pend_req(const void *addr) {
//   uint32_t bidx = bucket_hash(addr);
//   time_point cur = get_time();

//   // After halftrip, the request arrives at DRAM
//   cur += c2c_halftrip;

//   time_point queue_finish_point =
//       req_queue[bidx].load(std::memory_order_relaxed);

//   bool ret = false;
//   time_point until_time;

//   // Wait for the request to finish processing
//   while (!ret) {
//     until_time = std::max(queue_finish_point, cur) + ProcessTime;

//     // Use compare_exchange_weak for potential performance improvement.
//     // Use release semantics on success to make prior writes visible
//     // to other threads that acquire this atomic.
//     // Use acquire semantics on failure to ensure visibility of writes
//     // from the thread that caused the CAS to fail.
//     ret = req_queue[bidx].compare_exchange_weak(
//         queue_finish_point, until_time, std::memory_order_release,
//         std::memory_order_acquire);
//   }

//   // After processing and a halftrip, the request finished.
//   wait_until(until_time + c2c_halftrip);
// }

// void AtomicRequestQueue::pend_cas_req(const void *addr) {
//   pend_req<cas_process_time>(addr);
// }

// void AtomicRequestQueue::pend_load_req(const void *addr) {
//   pend_req<load_process_time>(addr);
// }

// void AtomicRequestQueue::pend_store_req(const void *addr) {
//   pend_req<store_process_time>(addr);
// }
