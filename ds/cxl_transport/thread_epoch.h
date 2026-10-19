#pragma once

#include <memory>
#include <atomic>

// epoch & rcu
class ThreadStatus {
 public:
  struct alignas(64) Status {
    std::atomic<uint64_t> epoch;
    std::atomic<bool> running;
    std::atomic<bool> safe_waiting;
  };

  class Guard {
    ThreadStatus& owner; int id;
   public:
    Guard(ThreadStatus& owner,int id):owner(owner),id(id) { owner.rcu_progress(id); }
    ~Guard() { owner.rcu_exit(id); }
  };
  ThreadStatus(int num_threads) : num_threads(num_threads) {
    status_set = std::make_unique<Status[]>(num_threads);
    for (int i = 0; i < num_threads; i++) {
      status_set[i].epoch = 0;
      status_set[i].running = false;
      status_set[i].safe_waiting = false;
    }
  }

  void rcu_progress(int worker_id) {
    status_set[worker_id].running = true;
    status_set[worker_id].epoch = status_set[worker_id].epoch + 1;
    // mfence();
  }

  // void rcu_safe_wait(int worker_id) {
  //   status_set[worker_id].safe_waiting = true;
  // }

  // void rcu_continue(int worker_id) {
  //   status_set[worker_id].safe_waiting = false;
  // }

  void rcu_exit(int worker_id) {
    status_set[worker_id].running = false;
  }

  void rcu_barrier() {
    uint64_t prev_status[num_threads];
    for (int i = 0; i < num_threads; i++) {
      prev_status[i] = status_set[i].epoch;
    }
    for (int i = 0; i < num_threads; i++) {
      while (status_set[i].running && prev_status[i] == status_set[i].epoch)
        ;
    }
  }

  uint64_t get_epoch(int worker_id) {
    return status_set[worker_id].epoch;
  }

 private:
  int num_threads;
  std::unique_ptr<Status[]> status_set;
};
