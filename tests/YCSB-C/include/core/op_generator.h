#pragma once

#include <vector>

#include "core/core_workload.h"
#include "core/db.h"
#include "core/ub_session.h"
#include "shm/cxl_type.h"

namespace ycsbc {
template <typename K, typename V>
class OpGenerator {
 public:
  OpGenerator(CoreWorkload* wl) : workload_(wl) {}
  virtual int GenerateRead(cxl_vector<OperationInternal<K, V>>& ops) = 0;
  virtual int GenerateReadModifyWrite(cxl_vector<OperationInternal<K, V>>& ops) = 0;
  virtual int GenerateScan(cxl_vector<OperationInternal<K, V>>& ops) = 0;
  virtual int GenerateUpdate(cxl_vector<OperationInternal<K, V>>& ops) = 0;
  virtual int GenerateInsert(cxl_vector<OperationInternal<K, V>>& ops) = 0;

  virtual cxl_vector<cxl_vector<OperationInternal<K, V>>> GenerateOperations(
      int num_ops, int group_size) {
    cxl_vector<cxl_vector<OperationInternal<K, V>>> ops(group_size);
    for (int i = 0; i < group_size; ++i) {
      // Reserve large enough space
      ops[i].reserve(1.1 * num_ops / group_size);
    }
    for (int i = 0; i < num_ops; ++i) {
#ifdef YCSB_UB_TWO_HOST
      // Keep the framework generator; only select the host and its read/write mix.
      const int machine=i<num_ops/2?0:1;
      int group=ycsbc_ub::first_thread(machine)+workload_->NextGroup(ycsbc_ub::threads(machine));
      auto operation=machine==1 && !std::getenv("YCSB_UB_SUITE")?READ:workload_->NextOperation();
#else
      int group = workload_->NextGroup(group_size);
      auto operation=workload_->NextOperation();
#endif
      switch (operation) {
        case READ:
          GenerateRead(ops[group]);
          break;
        case UPDATE:
          GenerateUpdate(ops[group]);
          break;
        case INSERT:
          GenerateInsert(ops[group]);
          break;
        case SCAN:
          GenerateScan(ops[group]);
          break;
        case READMODIFYWRITE:
          GenerateReadModifyWrite(ops[group]);
          break;
        default:
          throw utils::Exception("Operation request is not recognized!");
      }
    }
    return ops;
  }

  virtual ~OpGenerator() {}

 protected:
  CoreWorkload* workload_;
};
}  // namespace ycsbc
