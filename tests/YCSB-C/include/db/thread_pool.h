#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <random>

#include "msg/mpmc_queue.h"

namespace ycsbc {

class ThreadPool {
 public:
  ThreadPool(size_t num_threads, std::function<int()> alloc_task,
             std::function<void(int)> release_task);

  ThreadPool(size_t num_threads, size_t num_bucket,
             std::function<int()> alloc_task,
             std::function<void(int)> release_task,
             std::function<bool(size_t)> remote_poll = {},
             std::function<void(size_t)> worker_setup = {});

#ifdef TIMING_LOAD_BALANCE
  void init_timing_load_balance();
#endif

  template <class F>
  void enqueue(int i, F&& f) {
    while (!tasks[i % num_bucket].enqueue(std::forward<F>(f)));
    // thread_local std::mt19937 rng(std::random_device{}()); // 利用线程本地存储
    // std::uniform_int_distribution<int> dist(0, num_bucket - 1);
    
    // int index = dist(rng);
    // while (!tasks[index].enqueue(std::forward<F>(f))) {
    //     index = dist(rng); // 如果失败则生成新的随机索引
    // }
  }

  void thread_job(int machine_id, size_t thread_id,
                  std::function<int()> alloc_task,
                  std::function<void(int)> release_task);

  void close();

  ~ThreadPool();

#ifdef TIMING_LOAD_BALANCE
  std::atomic<bool> start_sample{false};
  std::atomic<bool> end_sample{false};
  static int pool_index;
#endif

 private:
  std::vector<std::thread> threads;
  std::vector<MPMCQueue<std::function<void()>>> tasks;

  std::function<bool(size_t)> remote_poll_;
  std::function<void(size_t)> worker_setup_;
  bool run_one(size_t bucket);
  std::atomic<bool> stop;

  size_t num_threads;
  size_t num_bucket;
  std::mutex config_task_mutex{};
#ifdef TIMING_LOAD_BALANCE
  std::atomic<int> request_count{0};
  std::thread timing_load_balance_thread;
#endif
};

}  // namespace ycsbc