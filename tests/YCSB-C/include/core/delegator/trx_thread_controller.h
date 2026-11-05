#pragma once
#include <sstream>
#include <atomic>
#include <vector>
#include <chrono>
#include <iomanip>
#include "core/ub_session.h"
#include "db/ub_db.h"

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
#ifdef YCSB_UB_TWO_HOST
template<class T> using ControllerAtomic=nt<T>;
#else
template<class T> using ControllerAtomic=std::atomic<T>;
#endif
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
#ifdef YCSB_UB_TWO_HOST
      thread_args_.push_back({this, i < unsigned(ycsbc_ub::threads(0)) ? 0U : 1U, i});
#else
      thread_args_.push_back({this,
                              i / (SimThreadInfo::dispatcher_thread_count /
                                   SimThreadInfo::worker_machine_count),
                              i});
#endif
    }
  }

  void StartThreads() {
    uint32_t mid = SimThreadInfo::worker_machine_id;
#ifdef YCSB_UB_TWO_HOST
    uint32_t first=ycsbc_ub::first_thread(mid), last=first+ycsbc_ub::threads(mid);
#else
    uint32_t dispatcher_per_machine = SimThreadInfo::dispatcher_thread_count /
                                      SimThreadInfo::worker_machine_count;
    uint32_t first=mid*dispatcher_per_machine, last=(mid+1)*dispatcher_per_machine;
#endif
    for (uint32_t i = first; i < last; i++) {
#ifndef SNIPER
      pthread_attr_t *attr = new pthread_attr_t;
      int cpu_id;
#if defined(YCSB_UB_TWO_HOST)
      cpu_id=0;
#elif defined(USE_MSG_QUEUE)
      // When using message queue, bind client threads to node 0, bind other thread pool threads to node 123
      cpu_id = cpu_allocator.allocate_cpu(0);
#else
      // When not using message queue, bind threads to node 123, allocate shared memory on node 0
      cpu_id = cpu_allocator.allocate_cpu(1);
#endif
#ifdef YCSB_UB_TWO_HOST
      cpu_id=ycsbc_ub::cpu_start()+i-first;
      cpu_set_t allowed, selected; CPU_ZERO(&selected);
      if(cpu_id<0 || cpu_id>=CPU_SETSIZE || sched_getaffinity(0,sizeof(allowed),&allowed) || !CPU_ISSET(cpu_id,&allowed))
        throw std::runtime_error("Requested UB worker CPU is unavailable");
      CPU_SET(cpu_id,&selected);
      pthread_attr_init(attr);
      if(pthread_attr_setaffinity_np(attr,sizeof(selected),&selected)) throw std::runtime_error("pthread affinity failed");
#else
      set_pthread_affinity_attr(cpu_id, attr);
#endif
      int rc=pthread_create(&threads_[i], attr,
                     &TransactionThreadController::DelegateClientTransWrapper,
                     (void *)&thread_args_[i]);
      pthread_attr_destroy(attr); delete attr;
      if(rc) throw std::runtime_error("pthread_create failed");
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
#ifdef YCSB_UB_TWO_HOST
    uint32_t first=ycsbc_ub::first_thread(mid), last=first+ycsbc_ub::threads(mid);
#else
    uint32_t dispatcher_per_machine = SimThreadInfo::dispatcher_thread_count /
                                      SimThreadInfo::worker_machine_count;
    uint32_t first=mid*dispatcher_per_machine, last=(mid+1)*dispatcher_per_machine;
#endif
    for (uint32_t i = first; i < last; i++) {
      void *tmp;
      int ret = pthread_join(threads_[i], &tmp);
      if (ret != 0) {
        std::cerr << "pthread_join failed, thread id: " << i << std::endl;
      }
      if (tmp != nullptr) {
        sum.fetch_add(*(int *)tmp);
#ifdef YCSB_UB_TWO_HOST
        local_success[mid].fetch_add(*(int *)tmp);
#endif
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

#ifdef YCSB_UB_TWO_HOST
  void WaitForLocalReady() {
    auto mid=SimThreadInfo::worker_machine_id;
    while(local_ready[mid].load()!=unsigned(ycsbc_ub::threads(mid))) std::this_thread::yield();
  }
  void WaitForAllReady() {
    for(int mid=0;mid<2;++mid)
      while(local_ready[mid].load()!=unsigned(ycsbc_ub::threads(mid))) std::this_thread::yield();
  }
  void ReportLocal(double seconds, uint64_t total=0) {
    auto mid=SimThreadInfo::worker_machine_id;
    uint64_t reads=0,writes=0,trace_hash=14695981039346656037ULL;
    for(int t=ycsbc_ub::first_thread(mid);t<ycsbc_ub::first_thread(mid)+ycsbc_ub::threads(mid);++t)
      for(const auto& op:op_groups[t]) {
        if(op.type==READ) ++reads; else ++writes;
        trace_hash=(trace_hash^uint64_t(t))*1099511628211ULL;
        trace_hash=(trace_hash^uint64_t(op.type))*1099511628211ULL;
        trace_hash=(trace_hash^uint64_t(op.key))*1099511628211ULL;
      }
    auto errors=reads+writes-local_success[mid].load();
    std::ostringstream response;
    response<<"RESULT {\"role\":\""<<(mid==0?"B":"A")
      <<"\",\"reads\":"<<reads<<",\"writes\":"<<writes<<",\"errors\":"<<errors
      <<",\"seconds\":"<<std::setprecision(12)<<seconds<<",\"framework_total_operations\":"<<total
      <<",\"bwtree_opt\":"
#ifdef OPT_IN_USE_FLAG
      <<"true"
#else
      <<"false"
#endif
      <<",\"rpc_completed\":"<<UbRpcCompleted()
      <<",\"trace_hash\":\""<<std::hex<<trace_hash<<"\"}";
    ycsbc_ub::reply(response.str());
  }
#endif
private:
  struct ThreadArgsDeleTrans {
    TransactionThreadController *manager;
    uint32_t sim_machine_id;
    uint32_t sim_thread_id;
  };

  cxl_vector<pthread_t> threads_;
  cxl_vector<ThreadArgsDeleTrans> thread_args_;
  ControllerAtomic<uint64_t> sum;
  cxl_vector<ycsbc::DB *> *dbs_;
  ControllerAtomic<int> sync_var_;
  cxl_vector<cxl_vector<Operation>> op_groups;
  cxl_vector<ControllerAtomic<int>> finish_flags;
#ifdef YCSB_UB_TWO_HOST
  nt<uint64_t> local_ready[2], local_success[2];
#endif
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
#ifdef YCSB_UB_TWO_HOST
    ycsbc_ub::stage("thread " + std::to_string(arg->sim_thread_id) + " registering with index");
#endif
    InitDelegateClient(thread_ids);
#ifdef YCSB_UB_TWO_HOST
    ycsbc_ub::stage("thread " + std::to_string(arg->sim_thread_id) + " registered; constructing Client");
#endif
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

#ifdef YCSB_UB_TWO_HOST
    ycsbc_ub::stage("thread " + std::to_string(arg->sim_thread_id) + " publishing ready flag");
    local_ready[arg->sim_machine_id].fetch_add(1);
    ycsbc_ub::stage("thread " + std::to_string(arg->sim_thread_id) + " ready flag published");
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
