#pragma once
#include <atomic>
#include <vector>
#include <chrono>

#include "connection/establish.h"
#include "core/client.h"
#include "core/db.h"
#include "core/db_config.h"
#include "core/op_generator_int_key.h"
#include "core/op_generator_int_key_addr.h"
#include "core/timer.h"
#include "follower/global_variable.h"
#include "shm/cxl_type.h"
#include "utils/cpu_dist.h"
#include "utils/sim_id.h"

namespace ycsbc {
#ifdef INT_KEY_ADDR
using Operation = OpGeneratorIntKeyAddr::Operation;
#elif defined(INT_YCSBC_KEY)
using Operation = OpGeneratorIntKey::Operation;
#elif defined(YCSB_KEY)
#endif

class TransactionThreadController {
public:
  TransactionThreadController(cxl_vector<ycsbc::DB *> *dbs,
                              cxl_vector<cxl_vector<Operation>> &&op_groups)
      : sum(0), dbs_(dbs), sync_var_(0), op_groups(std::move(op_groups)),
        finish_flags(SimThreadInfo::worker_machine_count) {}

  void PrepareThreads() {
    threads_.resize(SimThreadInfo::dispatcher_thread_count);
    thread_args_.reserve(SimThreadInfo::dispatcher_thread_count);
    for (uint32_t i = 0; i < SimThreadInfo::dispatcher_thread_count; ++i) {
      thread_args_.push_back({this,
                              i / (SimThreadInfo::dispatcher_thread_count /
                                   SimThreadInfo::worker_machine_count),
                              i});
    }
  }

  void StartThreads() {
    uint32_t mid = SimThreadInfo::worker_machine_id;
    uint32_t dispatcher_per_machine = SimThreadInfo::dispatcher_thread_count /
                                      SimThreadInfo::worker_machine_count;
    for (uint32_t i = mid * dispatcher_per_machine;
         i < (mid + 1) * dispatcher_per_machine; i++) {
#ifndef SNIPER
      pthread_attr_t *attr = new pthread_attr_t;
      int cpu_id;
#ifdef USE_MSG_QUEUE
      // When using message queue, bind client threads to node 0, bind other thread pool threads to node 123
      cpu_id = cpu_allocator.allocate_cpu(0);
#else
      // When not using message queue, bind threads to node 123, allocate shared memory on node 0
      cpu_id = cpu_allocator.allocate_cpu(1);
#endif
      set_pthread_affinity_attr(cpu_id, attr);
      pthread_create(&threads_[i], attr,
                     &TransactionThreadController::DelegateClientTransWrapper,
                     (void *)&thread_args_[i]);
#else
      pthread_create(&threads_[i], nullptr,
                     &TransactionThreadController::DelegateClientTransWrapper,
                     (void *)&thread_args_[i]);
#endif
    }
  }

  void StartExecution() { sync_var_.store(1, std::memory_order_release); }

  void WaitForLocalThreads() {
    uint32_t mid = SimThreadInfo::worker_machine_id;
    uint32_t dispatcher_per_machine = SimThreadInfo::dispatcher_thread_count /
                                      SimThreadInfo::worker_machine_count;
    for (uint32_t i = mid * dispatcher_per_machine;
         i < (mid + 1) * dispatcher_per_machine; i++) {
      void *tmp;
      int ret = pthread_join(threads_[i], &tmp);
      if (ret != 0) {
        std::cerr << "pthread_join failed, thread id: " << i << std::endl;
      }
      if (tmp != nullptr) {
        sum += *(int *)tmp;
        delete (int *)tmp;
      } else {
        std::cerr << "pthread_join returned nullptr, thread id: " << i << std::endl;
      }
    }
    finish_flags[SimThreadInfo::worker_machine_id].store(
        1, std::memory_order_release);
  }

  void CollectResults(uint64_t &sum) {
    for (size_t i = 0; i < finish_flags.size(); i++) {
      while (!finish_flags[i].load(std::memory_order_acquire))
        ;
    }
    for (size_t i = 0; i < dbs_->size(); i++) {
      (*dbs_)[i]->Close();
    }

    sum = this->sum.load(std::memory_order_acquire);
  }

private:
  struct ThreadArgsDeleTrans {
    TransactionThreadController *manager;
    uint32_t sim_machine_id;
    uint32_t sim_thread_id;
  };

  cxl_vector<pthread_t> threads_;
  cxl_vector<ThreadArgsDeleTrans> thread_args_;
  std::atomic<uint64_t> sum;
  cxl_vector<ycsbc::DB *> *dbs_;
  std::atomic<int> sync_var_;
  cxl_vector<cxl_vector<Operation>> op_groups;
  cxl_vector<std::atomic<int>> finish_flags;
#ifdef ASYNC_CLIENT
  // The number of requests that a client can send simultaneously
  const int virtual_clients = 128;
#endif

  static void *DelegateClientTransWrapper(void *context) {
    ThreadArgsDeleTrans *arg = (ThreadArgsDeleTrans *)context;
    return arg->manager->DelegateClientTrans(context);
  }

  void *DelegateClientTrans(void *context) {
    ThreadArgsDeleTrans *arg = (ThreadArgsDeleTrans *)context;
    std::vector<int> thread_ids;
    // auto thread_args = reinterpret_cast<ThreadArgsDeleTrans *>(arg);
#ifdef USE_MSG_QUEUE
    SimThreadInfo::setup_dispatcher_id(arg->sim_thread_id);
#else
    SimThreadInfo::setup_worker_ids(arg->sim_machine_id, arg->sim_thread_id);
#endif
    InitDelegateClient(thread_ids);
#ifdef TIMING_LOAD_BALANCE
    for (auto db : *thread_args->dbs) {
      db->pool->start_sample = true;
    }
#endif
    Client client(*dbs_
#ifdef USE_CONSISTENT_HASH
                  ,
                  ring_
#endif
    );

    // check thread affinity
#if 0
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  for (int i = 0; i < CPU_SETSIZE; i++) {
      if (CPU_ISSET(i, &cpuset)) {
          printf("thread is running on cpu %d\n", i);
      }
  }
#endif

    while (sync_var_.load(std::memory_order_acquire) != 1) {
      // wait for the global state to be set
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    int oks = 0;
    cxl_vector<Operation> &ops = op_groups[arg->sim_thread_id];
#ifdef ASYNC_CLIENT
    // Store the value of read for check
    std::vector<uint64_t> read_value(virtual_clients);
    std::vector<std::atomic<int>> finish(virtual_clients);
    std::vector<std::atomic<int>> finish_flag(10000000);
    for (int i = 0; i < virtual_clients; ++i) finish[i].store(1, std::memory_order_relaxed);
    std::vector<int> result(virtual_clients);
    int last_pos = arg->sim_thread_id % virtual_clients;
    for (size_t i = 0; i < ops.size(); i++) {
      while (!finish[last_pos].load(std::memory_order_acquire)) {
        last_pos = (last_pos + 1) % virtual_clients;
      }
#ifdef INT_KEY_ADDR
      uint64_t *addr = reinterpret_cast<uint64_t *>(read_value[last_pos]);
      if (addr != nullptr) {
        size_t size = *addr;
        for (size_t i = 1; i < size / sizeof(uint64_t); i++) {
          if (addr[i] != size) {
            std::cout << "Error: Value not correct" << std::endl;
            break;
          }
        }
      }
      read_value[last_pos] = 0;
#endif
      int pos = 0;
      finish_flag[pos].store(0, std::memory_order_relaxed);
      oks += client.DoTransaction(ops[i], finish_flag[pos], result[last_pos],
                                  read_value[last_pos]);
      #if defined(USE_MWAIT) && defined(__x86_64__) && defined(__WAITPKG__)
      _umonitor(&finish_flag[pos]);
      while (finish_flag[pos].load(std::memory_order_acquire) == 0) {
        _umwait(0, 1);
      }
      #else
      while (finish_flag[pos].load(std::memory_order_acquire) == 0);
      #endif
      assert(finish_flag[pos].load(std::memory_order_acquire) == 1);
    }
#else
    for (size_t i = 0; i < ops.size(); i++) {
      oks += client.DoTransaction(ops[i]);
    }
#endif

    CloseDelegateClient(thread_ids);
#ifdef TIMING_LOAD_BALANCE
    for (auto db : *dbs_) {
      db->pool->end_sample = true;
    }
#endif
    int *oks_ptr = new int(oks);
    return oks_ptr;
  }

  void InitDelegateClient(std::vector<int> &thread_ids) {
#ifndef USE_MSG_QUEUE
    thread_ids.reserve(dbs_->size());
    for (auto db : *dbs_) {
      thread_ids.push_back(db->ThreadInit());
    }
#endif
  }

  void CloseDelegateClient(std::vector<int> &thread_ids) {
#ifndef USE_MSG_QUEUE
    for (size_t i = 0; i < dbs_->size(); i++) {
      (*dbs_)[i]->ThreadClose(thread_ids[i]);
    }
#endif
  }
};
} // namespace ycsbc