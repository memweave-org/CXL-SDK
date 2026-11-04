#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

#include "connection/establish.h"
#include "core/client.h"
#include "core/core_workload.h"
#include "core/db.h"
#include "core/db_config.h"
#include "core/delegator/trx_thread_controller.h"
#include "core/op_generator_int_key.h"
#include "core/op_generator_int_key_addr.h"
#include "core/real_workload.h"
#include "core/timer.h"
#include "follower/command.h"
#include "follower/global_variable.h"
#include "shm/cxl_type.h"
#include "shm/mm.h"
#include "utils/cpu_dist.h"
#include "utils/helper.h"
#include "utils/sim_id.h"

namespace ycsbc {
#ifdef INT_KEY_ADDR
using Operation = OpGeneratorIntKeyAddr::Operation;
#elif defined(INT_YCSBC_KEY)
using Operation = OpGeneratorIntKey::Operation;
#elif defined(YCSB_KEY)
#endif

class MasterTransactionManager {
public:
  MasterTransactionManager(std::vector<std::string> host,
                           cxl_vector<ycsbc::DB *> *dbs, CoreWorkload *wl,
                           uint64_t total_ops)
      : follower_manager_(host) {
#ifdef INT_KEY_ADDR
    OpGeneratorIntKeyAddr op_gen(wl);
#elif defined(INT_YCSBC_KEY)
    OpGeneratorIntKey op_gen(wl);
#elif defined(YCSB_KEY)
#endif

    auto ops = op_gen.GenerateOperations(
        total_ops, SimThreadInfo::dispatcher_thread_count);
    trx_thread_controller_ = ALLOC_AND_CONSTRUCT(
        TransactionThreadController, shared_atomic_memory().malloc, dbs, std::move(ops));
  }

  void Execute(utils::Properties &props, double &duration, uint64_t &sum) {
    trx_thread_controller_->PrepareThreads();
    if (stoi(props.GetProperty("machinenum", "1")) > 1) {
      WakeupFollowers(props);
    }
    trx_thread_controller_->StartThreads();
#ifdef YCSB_UB_TWO_HOST
    trx_thread_controller_->WaitForLocalReady();
    ycsbc_ub::ready();
    trx_thread_controller_->WaitForAllReady();
#endif

#ifdef SNIPER
    SimRoiStart();
#endif
    timer_.Start();

    trx_thread_controller_->StartExecution();
    // fprintf(stderr, "master thread sleep for 120 s\n");
    // std::this_thread::sleep_for(std::chrono::seconds(120));
    trx_thread_controller_->WaitForLocalThreads();
    trx_thread_controller_->CollectResults(sum);

#ifdef SNIPER
    SimRoiEnd();
#endif
    duration = timer_.End();
#ifdef YCSB_UB_TWO_HOST
    trx_thread_controller_->ReportLocal(duration, sum);
#endif
  }

  ~MasterTransactionManager() {
    DESTROY_AND_DEALLOC(trx_thread_controller_, TransactionThreadController,
                        shared_atomic_memory().free);
  }

private:
  TransactionThreadController *trx_thread_controller_;
  utils::Timer<double> timer_;
  FollowerManager follower_manager_;
#ifdef ASYNC_CLIENT
  // The number of requests that a client can send simultaneously
  const int virtual_clients = 128;
#endif

  void WakeupFollowers(utils::Properties &props) {
#ifdef YCSB_UB_TWO_HOST
    g_var_struct = new (ycsbc_ub::control_header()) GlobalVariables{};
    g_var_struct->trx_thread_controller = trx_thread_controller_;
    mfence();
#else
    void *tmp_g_var_addr = shared_atomic_memory().malloc(sizeof(GlobalVariables));
    g_var_struct = new (tmp_g_var_addr) GlobalVariables();
    g_var_struct->trx_thread_controller = trx_thread_controller_;
    follower_manager_.start_followers([&props](uint32_t id) -> std::string {
      return CommandBuilder::build(props, id);
    });
#endif
  }
};
} // namespace ycsbc