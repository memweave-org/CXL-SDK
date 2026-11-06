#include "db/thread_pool.h"

#include <atomic>
#include <cassert>
#include <fstream>
#include <iostream>
#include <random>

#include "core/utils.h"
#include "utils/compiler.h"
#include "utils/config.h"
#include "utils/cpu_dist.h"
#include "utils/sim_id.h"

#define THREAD_POOL_EXIT_TIME

namespace ycsbc {

ThreadPool::ThreadPool(size_t num_threads, std::function<int()> alloc_task,
                       std::function<void(int)> release_task)
    : ThreadPool(num_threads, num_threads, alloc_task, release_task) {}

ThreadPool::ThreadPool(size_t num_threads, size_t num_bucket,
                       std::function<int()> alloc_task,
                       std::function<void(int)> release_task,
                       std::function<bool(size_t)> remote_poll,
                       std::function<void(size_t)> worker_setup)
    : remote_poll_(std::move(remote_poll)), worker_setup_(std::move(worker_setup)),
      stop(false), num_threads(num_threads), num_bucket(num_bucket) {
  if (!num_threads || !num_bucket) throw std::invalid_argument("Empty MQ thread pool");
  tasks.reserve(num_bucket);
#ifdef TIMING_LOAD_BALANCE
  init_timing_load_balance();
#endif
  for (size_t i = 0; i < num_bucket; i++) {
    tasks.emplace_back(8192);
  }
  const int machine_id = SimThreadInfo::worker_machine_id;
  for (size_t i = 0; i < num_threads; ++i) {
    threads.emplace_back([this, i, machine_id, alloc_task, release_task] {
      thread_job(machine_id, i, alloc_task, release_task);
    });
  }
}

#ifdef TIMING_LOAD_BALANCE
void ThreadPool::init_timing_load_balance() {
  timing_load_balance_thread = std::thread([this]() -> void {
    int local_pool_index = pool_index++;
    std::vector<int> result;
    while (!start_sample)
      ;
    while (!end_sample) {
      result.emplace_back(this->request_count);
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
    std::string file_name = std::string("./log/lb/") +
                            std::to_string(local_pool_index) +
                            std::string("_sample.log");
    std::ofstream out_file(file_name);
    if (!out_file) {
      return;
    }

    for (const auto &num : result) {
      out_file << num << " ";
    }
    out_file << "\n";
    out_file.close();
  });
}
#endif

inline void relax_fence() {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    asm volatile("pause" : : : "memory");  // equivalent to "rep; nop"
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" : : : "memory");  // ARM equivalent to pause
#else
    asm volatile("" : : : "memory");  // fallback for other architectures
#endif
}

void ThreadPool::thread_job(int machine_id, size_t thread_id,
                            std::function<int()> alloc_task,
                            std::function<void(int)> release_task) {
  // We assume that only one thread pool will be working at one time, to reduce
  // the effort to manage all kinds of thread id
  SimThreadInfo::setup_worker_ids(machine_id, thread_id);
  if (worker_setup_) worker_setup_(thread_id);
  else set_pthread_affinity(cpu_allocator.allocate_cpu(1));
  std::vector<uint32_t> bucket_order(num_bucket);
  for (size_t i = 0; i < num_bucket; ++i) {
    bucket_order[i] = i;
  }
  std::shuffle(bucket_order.begin(), bucket_order.end(),
               std::mt19937(std::random_device()()));

  int alloc_thread_id = -1;
  {
    std::unique_lock<std::mutex> lock(this->config_task_mutex);
    alloc_thread_id = alloc_task();
  }

  int order_index = 0;
  int last_pos = bucket_order[order_index];
  while (true) {
    if (run_one(last_pos)) {
#ifdef TIMING_LOAD_BALANCE
      if (start_sample) {
        request_count++;
      }
#endif
    } else {
      if (this->stop.load(std::memory_order_relaxed)) [[unlikely]] {
        for (size_t corr_bucket = thread_id % num_bucket;
             corr_bucket < num_bucket; corr_bucket += num_threads) {
          while (run_one(corr_bucket)) {
#ifdef TIMING_LOAD_BALANCE
            if (start_sample) {
              request_count++;
            }
#endif
            relax_fence();
          }
        }
        std::unique_lock<std::mutex> lock(this->config_task_mutex);
        if (alloc_thread_id != -1)
          release_task(alloc_thread_id);
        return;
      }
      order_index = (order_index + 1) % num_bucket;
      last_pos = bucket_order[order_index];
      relax_fence();
    }
  }
}

bool ThreadPool::run_one(size_t bucket) {
  // Service both sources per visit so local traffic cannot starve a peer.
  bool worked = remote_poll_ && remote_poll_(bucket);
  std::function<void()> task;
  if (tasks[bucket].try_dequeue(task)) { task(); worked = true; }
  return worked;
}

void ThreadPool::close() {
  if (stop.exchange(true)) return;
#ifdef TIMING_LOAD_BALANCE
  timing_load_balance_thread.join();
#endif
  for (std::thread &worker : threads) {
    worker.join();
  }
}

ThreadPool::~ThreadPool() {
  if (stop == false) {
    close();
  }
}

#ifdef TIMING_LOAD_BALANCE
int ThreadPool::pool_index = 0;
#endif

} // namespace ycsbc