#include "core/insert_client.h"

#include "core/consistent_hash.h"
#include "core/op_generator_int_key_addr.h"
#include "core/perf.h"
#include <cstdint>
#include <memory>
#include <atomic>

namespace ycsbc {
int InsertClient::Task(Operation &op) { return 0; }

bool InsertClient::DoInsert() {
#ifdef TRX_LATENCY
  TimerAverage timer("insert_load");
  timer.Start();
#endif
  bool ret;
#ifdef INT_YCSBC_KEY
  uint64_t key = workload_->NextSequenceNumKey();
  memory_fence();
  ret = (db_[get_db(key)]->Insert(key, key) == DB::kOK);
#elif defined(INT_KEY_ADDR)
  OpGeneratorIntKeyAddr op_gen(workload_);
  uint64_t key = workload_->NextSequenceNumKey();
#ifdef YCSB_UB_TWO_HOST
  if(ycsbc_ub::rpc_mode() && UbRpcOwner(key)!=SimThreadInfo::worker_machine_id)
    return false; // Not a local record; neither allocate nor RPC during prefill.
#endif
  void *value = workload_->BuildPrefillValue();
  memory_fence();
#ifdef YCSB_UB_TWO_HOST
  if(ycsbc_ub::rpc_mode()) return UbPrefillLocal(db_[get_db(key)],key,reinterpret_cast<uintptr_t>(value))==DB::kOK;
#endif
  ret = (db_[get_db(key)]->Insert(key, (uint64_t)value) == DB::kOK);
#elif defined(YCSB_KEY)
  std::string key = workload_->NextSequenceKey();
  std::vector<DB::KVPair> pairs;
  workload_->BuildValues(pairs);
  memory_fence();
  ret = (db_[get_db(key)]->Insert(workload_->NextTable(), key, pairs) ==
          DB::kOK);
#endif
#ifdef TRX_LATENCY
  timer.Stop();
#endif
  return ret;
}

#ifdef ASYNC_CLIENT
int InsertClient::TransactionRead(Operation &op, std::atomic<int> &finish, int &return_value, uint64_t& read_value) {
  std::vector<DB::KVPair> result;
  int ret;
#ifdef TRX_LATENCY
  TimerAverage timer("read_latency");
  timer.Start();
#endif
#ifdef YCSB_KEY
  if (!workload_->read_all_fields()) {
    ret = db_[get_db(op.key)]->Read(op.table, op.key, &op.fields, result);
  } else {
    ret = db_[get_db(op.key)]->Read(op.table, op.key, NULL, result);
  }
#elif defined(INT_YCSBC_KEY)
  ret = db_[get_db(op.key)]->AsyncRead(op.key, read_value, finish, return_value);
#elif defined(INT_KEY_ADDR)
  ret = db_[get_db(op.key)]->AsyncRead(op.key, read_value, finish,
                                       return_value);
#endif
#ifdef TRX_LATENCY
  timer.Stop();
#endif
  return ret;
}

int InsertClient::TransactionReadModifyWrite(Operation &op, std::atomic<int> &finish,
                                       int &return_value) {
  std::vector<DB::KVPair> result;
#ifdef YCSB_KEY
  if (!workload_->read_all_fields()) {
    db_[get_db(op.key)]->Read(op.table, op.key, &op.fields, result);
  } else {
    db_[get_db(op.key)]->Read(op.table, op.key, NULL, result);
  }
  return db_[get_db(op.key)]->Update(op.table, op.key, op.value);
#elif defined(INT_YCSBC_KEY)
  return db_[get_db(op.key)]->AsyncUpdate(op.key, op.value, finish, return_value);
#elif defined(INT_KEY_ADDR)
  return db_[get_db(op.key)]->AsyncUpdate(op.key, (uint64_t)op.value, finish,
                                          return_value);
#endif
}

int InsertClient::TransactionScan(Operation &op, std::atomic<int> &finish, int &return_value) {
  int ret = 0;
#ifdef USE_CONSISTENT_HASH
  uint32_t db_index = get_db(op.key);
  uint32_t element_nr = op.len;
  uint32_t cur_element_nr = 0;
  for (; cur_element_nr < element_nr; db_index = ring_.NextNode(db_index)) {
    if (!workload_->read_all_fields()) {
      ret = db_[db_index]->Scan(op.table, op.key, op.len, &op.fields, result,
                                ids_);
    } else {
      ret = db_[db_index]->Scan(op.table, op.key, op.len, NULL, result);
    }
    cur_element_nr += result.size();
  }
#else
  throw "Scan: function not implemented!";
#endif
  return ret;
}

int InsertClient::TransactionUpdate(Operation &op, std::atomic<int> &finish, int &return_value) {
  int ret;
#ifdef TRX_LATENCY
  TimerAverage timer("update_latency");
  timer.Start();
#endif
#ifdef YCSB_KEY
  ret = db_[get_db(op.key)]->Update(op.table, op.key, op.value);
#elif defined(INT_YCSBC_KEY)
  ret = db_[get_db(op.key)]->AsyncUpdate(op.key, op.value, finish, return_value);
#elif defined(INT_KEY_ADDR)
  ret = db_[get_db(op.key)]->AsyncUpdate(op.key, (uint64_t)op.value, finish,
                                         return_value);
#endif
#ifdef TRX_LATENCY
  timer.Stop();
#endif
  return ret;
}

int InsertClient::TransactionInsert(Operation &op, std::atomic<int> &finish, int &return_value) {
  int ret;
#ifdef TRX_LATENCY
  TimerAverage timer("insert_latency");
  timer.Start();
#endif
#ifdef YCSB_KEY
  ret = db_[get_db(op.key)]->Insert(op.table, op.key, op.value);
#elif defined(INT_YCSBC_KEY)
  ret = db_[get_db(op.key)]->AsyncInsert(op.key, op.value, finish, return_value);
#elif defined(INT_KEY_ADDR)
  ret = db_[get_db(op.key)]->AsyncInsert(op.key, (uint64_t)op.value, finish, return_value);
#endif
#ifdef TRX_LATENCY
  timer.Stop();
#endif
  return ret;
}
#else
int InsertClient::TransactionRead(Operation &op) {
  int ret;
#ifdef TRX_LATENCY
  TimerAverage timer("read_latency");
  timer.Start();
#endif
#ifdef YCSB_KEY
  std::vector<DB::KVPair> result;
  if (!workload_->read_all_fields()) {
    ret = db_[get_db(op.key)]->Read(op.table, op.key, &op.fields, result);
  } else {
    ret = db_[get_db(op.key)]->Read(op.table, op.key, NULL, result);
  }
#else
  uint64_t value;
  ret = db_[get_db(op.key)]->Read(op.key, value);
#ifdef INT_KEY_ADDR
  // if (ret == DB::kOK) {
  //   uint64_t *addr = reinterpret_cast<uint64_t *>(value);
  //   uint64_t size = *addr;
  //   for (size_t i = 1; i < size / sizeof(uint64_t); i++) {
  //     if (addr[i] != size) {
  //       return DB::kErrorNoData;
  //     }
  //   }
    // std::unique_ptr<char[]> target_buffer = std::make_unique<char[]>(size);
    // std::memcpy(target_buffer.get(), addr, size);
    // uint64_t *value_ptr = reinterpret_cast<uint64_t *>(target_buffer.get());
    // for (size_t i = 1; i < size / sizeof(uint64_t); i++) {
    //   if (value_ptr[i] != size) {
    //     return DB::kErrorNoData;
    //   }
    // }
  // }
#endif
#endif
#ifdef TRX_LATENCY
  timer.Stop();
#endif
  return ret;
}

int InsertClient::TransactionReadModifyWrite(Operation &op) {
#ifdef YCSB_KEY
  std::vector<DB::KVPair> result;
  if (!workload_->read_all_fields()) {
    db_[get_db(op.key)]->Read(op.table, op.key, &op.fields, result);
  } else {
    db_[get_db(op.key)]->Read(op.table, op.key, NULL, result);
  }
  return db_[get_db(op.key)]->Update(op.table, op.key, op.value);
#else
  uint64_t value;
  db_[get_db(op.key)]->Read(op.key, value);
  return db_[get_db(op.key)]->Update(op.key, (uint64_t)op.value);
#endif
}

int InsertClient::TransactionScan(Operation &op) {
  int ret = 0;
#ifdef USE_CONSISTENT_HASH
  uint32_t db_index = get_db(op.key);
  uint32_t element_nr = op.len;
  uint32_t cur_element_nr = 0;
  for (; cur_element_nr < element_nr; db_index = ring_.NextNode(db_index)) {
    if (!workload_->read_all_fields()) {
      ret = db_[db_index]->Scan(op.table, op.key, op.len, &op.fields, result,
                                ids_);
    } else {
      ret = db_[db_index]->Scan(op.table, op.key, op.len, NULL, result);
    }
    cur_element_nr += result.size();
  }
#else
  uint32_t db_index = get_db(op.key);
  std::vector<std::vector<DB::KVPair>> result;
  return db_[db_index]->ScanInternal(op.key, op.len, result);
#endif
  return ret;
}

int InsertClient::TransactionUpdate(Operation &op) {
  int ret;
  
#ifdef TRX_LATENCY
  TimerAverage timer("update_latency");
  timer.Start();
#endif
#ifdef YCSB_KEY
  ret = db_[get_db(op.key)]->Update(op.table, op.key, op.value);
#else
  ret = db_[get_db(op.key)]->Update(op.key, (uint64_t)op.value);
#endif
#ifdef TRX_LATENCY
  timer.Stop();
#endif
  return ret;
}

int InsertClient::TransactionInsert(Operation &op) {
  int ret;
#ifdef TRX_LATENCY
  TimerAverage timer("insert_latency");
  timer.Start();
#endif
#ifdef YCSB_KEY
  ret = db_[get_db(op.key)]->Insert(op.table, op.key, op.value);
#else
  ret = db_[get_db(op.key)]->Insert(op.key, (uint64_t)op.value);
#endif
#ifdef TRX_LATENCY
  timer.Stop();
#endif
  return ret;
}
#endif

void InsertClient::do_test() {
  for (int i = 0; i < db_num; i++) {
    db_[i]->ThreadInit();
  }
  std::vector<DB::KVPair> pairs;
  std::vector<std::string> fie;
  std::string table = "user";
  for (int i = 0; i < 100000; i++) {
    db_[get_db(i)]->Insert(table, std::to_string(i), pairs);
  }
  for (int i = 0; i < 100000; i++) {
    db_[get_db(i)]->Read(table, std::to_string(i), &fie, pairs);
  }
}

}  // namespace ycsbc
