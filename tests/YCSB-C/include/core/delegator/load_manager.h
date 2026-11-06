#pragma once
#include "core/core_workload.h"
#include "core/db.h"
#include "core/ub_session.h"
#include "core/insert_client.h"
#include "shm/cxl_type.h"
#include <cassert>
#include <future>
#include <iostream>
#include <vector>

namespace ycsbc {
class LoadManager {
public:
  LoadManager(cxl_vector<DB *> *dbs, CoreWorkload *wl) : dbs_(dbs), wl_(wl) {}

  int LoadData(int dispatcher_nr, int machine_nr, int total_ops) {
    std::vector<std::future<int>> actual_ops;
    int cur_op_count = 0;
    int loader_nr = std::min(dispatcher_nr, 16);
#ifdef YCSB_UB_TWO_HOST
    if(ycsbc_ub::rpc_mode()) loader_nr=std::min(ycsbc_ub::threads(SimThreadInfo::worker_machine_id),16);
#endif
    cur_op_count = total_ops / loader_nr;
    for (int i = 0; i < loader_nr; ++i) {
#ifdef YCSB_UB_TWO_HOST
      const unsigned machine=ycsbc_ub::rpc_mode() ? SimThreadInfo::worker_machine_id : 0;
      IDPair ids(machine, ycsbc_ub::first_thread(machine)+i);
#else
      IDPair ids(i / (loader_nr / machine_nr), i);
#endif
      actual_ops.emplace_back(std::async(std::launch::async,
                                         &LoadManager::DelegateLoadWorker, this,
                                         cur_op_count + (i < total_ops % loader_nr), ids));
    }
    assert((int)actual_ops.size() == loader_nr);

    int sum = 0;
    for (auto &n : actual_ops) {
      assert(n.valid());
      sum += n.get();
    }
    std::cerr << "# Loaded records:\t" << sum << std::endl;
    actual_ops.clear();

    return sum;
  }

private:
  void InitDelegateClient(std::vector<int> &thread_ids) {
#if !defined(USE_MSG_QUEUE) || defined(YCSB_UB_TWO_HOST)
    thread_ids.reserve(dbs_->size());
    for (auto db : *dbs_) {
      thread_ids.push_back(db->ThreadInit());
      db->Init();
    }
#endif
  }

  void CloseDelegateClient(const std::vector<int> &thread_ids) {
#if !defined(USE_MSG_QUEUE) || defined(YCSB_UB_TWO_HOST)
    for (size_t i = 0; i < dbs_->size(); i++) {
      (*dbs_)[i]->ThreadClose(thread_ids[i]);
      (*dbs_)[i]->Close();
    }
#endif
  }

  int DelegateLoadWorker(const int num_ops, IDPair ids) {
    std::vector<int> thread_ids;
#if defined(USE_MSG_QUEUE) && !defined(YCSB_UB_TWO_HOST)
    SimThreadInfo::setup_dispatcher_id(ids.thread_id);
#else
    SimThreadInfo::setup_worker_ids(ids.machine_id, ids.thread_id);
#endif
    InitDelegateClient(thread_ids);

    InsertClient client(*dbs_, wl_);

    int oks = 0;
    for (int i = 0; i < num_ops; ++i) {
      oks += client.DoInsert();
    }

    for (auto db : *dbs_) {
      db->InitStats();
    }

    CloseDelegateClient(thread_ids);
    return oks;
  }

  cxl_vector<DB *> *dbs_;
  CoreWorkload *wl_;
};
} // namespace ycsbc
