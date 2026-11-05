//
//  client.h
//  YCSB-C
//
//  Created by Jinglei Ren on 12/10/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#pragma once

#include <cstring>
#include <string>
#include <vector>
#include <atomic>

#include "core/consistent_hash.h"
#include "core_workload.h"
#include "db.h"
#include "utils.h"
#include "utils/config.h"
#include "db/thread_pool.h"
#include "shm/cxl_type.h"
#include "db/ub_db.h"

namespace ycsbc {
class InsertClient {
#ifdef INT_YCSBC_KEY
  using Operation = ycsbc::OperationInternal<uint64_t, uint64_t>;
#elif defined(INT_KEY_ADDR)
  using Operation = ycsbc::OperationInternal<uint64_t, void*>;
#else
  using Operation = ycsbc::Operation<std::string, std::vector<DB::KVPair>>;
#endif
 public:
#ifdef USE_CONSISTENT_HASH
  InsertClient(std::vector<DB *> &db, HashRing &ring, CoreWorkload *wl)
      : db_(db), db_num(db.size()), ring_(ring), workload_(wl) {
      }
#else
  InsertClient(cxl_vector<DB *> &db, CoreWorkload *wl)
#ifdef YCSB_UB_TWO_HOST
      : db_num(db.size()), workload_(wl) {
        for(auto* descriptor:db) db_.push_back(LocalUbDB(descriptor));
      }
#else
      : db_(db), db_num(db.size()), workload_(wl) {
      }
#endif
#endif

  virtual bool DoInsert();
  // virtual bool DoInsertNumKey();
#ifdef ASYNC_CLIENT
  virtual bool DoTransaction(Operation& op, std::atomic<int>& finish, int& result, uint64_t& read_value){
    int status = -1;
    switch (op.type) {
      case READ:
        status = TransactionRead(op, finish, result, read_value);
        break;
      case UPDATE:
        status = TransactionUpdate(op, finish, result);
        break;
      case INSERT:
        status = TransactionInsert(op, finish, result);
        break;
      case SCAN:
        status = TransactionScan(op, finish, result);
        break;
      case READMODIFYWRITE:
        status = TransactionReadModifyWrite(op, finish, result);
        break;
      default:
        throw utils::Exception("Operation request is not recognized!");
    }
    return (status == DB::kOK);
  }
#else
  virtual bool DoTransaction(Operation& op) {
    int status = -1;
    switch (op.type) {
      case READ:
        status = TransactionRead(op);
        break;
      case UPDATE:
        status = TransactionUpdate(op);
        break;
      case INSERT:
        status = TransactionInsert(op);
        break;
      case SCAN:
        status = TransactionScan(op);
        break;
      case READMODIFYWRITE:
        status = TransactionReadModifyWrite(op);
        break;
      default:
        throw utils::Exception("Operation request is not recognized!");
    }
    return (status == DB::kOK);
  }
#endif

  virtual void do_test();

  virtual ~InsertClient() {
  }

 protected:
#ifdef ASYNC_CLIENT
  virtual int TransactionRead(Operation &op, std::atomic<int> &finish, int &result, uint64_t& read_value);
  virtual int TransactionReadModifyWrite(Operation &op, std::atomic<int> &finish,
                                         int &result);
  virtual int TransactionScan(Operation &op, std::atomic<int> &finish, int &result);
  virtual int TransactionUpdate(Operation &op, std::atomic<int> &finish, int &result);
  virtual int TransactionInsert(Operation &op, std::atomic<int> &finish, int &result);
#else
  virtual int TransactionRead(Operation &op);
  virtual int TransactionReadModifyWrite(Operation &op);
  virtual int TransactionScan(Operation &op);
  virtual int TransactionUpdate(Operation &op);
  virtual int TransactionInsert(Operation &op);
#endif
  
  virtual int Task(Operation &op);

  uint64_t hash(const std::string &key) {
    const char *key_str = key.c_str();
    size_t len = key.length();
    size_t i = 0;
    uint64_t hash = utils::kFNVOffsetBasis64;
    while (i + 8 <= len) {
      hash ^= *(uint64_t *)(key_str + i);
      hash *= utils::kFNVPrime64;
      i += 8;
    }
    if (i != len) {
      uint64_t chunk = 0;
      memcpy(&chunk, key_str + i, len - i);
      hash ^= chunk;
      hash *= utils::kFNVPrime64;
    }
    return hash;
  }

  inline uint32_t get_db(const std::string &key) {
#ifdef USE_CONSISTENT_HASH
    // return ring_.GetNode(hash(key));
    return JumpConsistentHash(hash(key), db_num);
#else
    return hash(key) % db_num;
#endif
  }

  inline uint32_t get_db(const int &key) {
#ifdef USE_CONSISTENT_HASH
    return ring_.GetNode(utils::Hash(key));
    // return JumpConsistentHash(utils::Hash(key), db_num);
#else
    return utils::Hash(key) % db_num;
#endif
  }

#ifdef USE_CONSISTENT_HASH
  HashRing &ring_;
#endif
#ifdef YCSB_UB_TWO_HOST
  std::vector<DB *> db_;
#else
  cxl_vector<DB *> &db_;
#endif
  int db_num;
  CoreWorkload *workload_;
};
}  // namespace ycsbc
