//
//  db.h
//  YCSB-C
//
//  Created by Jinglei Ren on 12/10/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#ifndef YCSB_C_DB_H_
#define YCSB_C_DB_H_

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

#include "core/db_config.h"
#include "core/utils.h"
#include "db/thread_pool.h"

namespace ycsbc {
class IDPair {
 public:
  int machine_id;
  int thread_id;

  IDPair(const int &mid, const int &tid) : machine_id(mid), thread_id(tid) {}
};

class DB {
 public:
#ifndef YCSB_KEY
  typedef std::pair<std::string, std::string> KVPair;
#else
  typedef std::pair<std::string, std::string> KVPair;
#endif
  static const int kOK = 0;
  static const int kErrorNoData = 1;
  static const int kErrorConflict = 2;
  ///
  /// Initializes any state for accessing this DB.
  /// Called once per DB client (thread); there is a single DB instance
  /// globally.
  ///
  virtual void Init() {}
  ///
  /// Clears any state for accessing this DB.
  /// Called once per DB client (thread); there is a single DB instance
  /// globally.
  ///
  virtual void Close() {}
  virtual int ThreadInit() { return 0; }
  virtual void ThreadClose(int /*thread_id*/) {}
  virtual int PoolThreadInit() { return 0; };
  virtual void PoolThreadClose(int thread_id) {};

  template <typename Func, typename... Args>
  int SyncExecute(Func func, Args &&...args) {
#ifdef USE_MSG_QUEUE
    std::atomic<int> finish{false};
    int result = 0;
    auto task = [this, &finish, &result, func, &args...]() {
      result = func(std::forward<Args>(args)...);
      finish = true;
    };
    pool->enqueue(SimThreadInfo::dispatcher_thread_id, task);
#ifdef RETURN_SYNC
    Poll(&finish);
    return result;
#else
    return DB::kOK;
#endif
#else
    return func(std::forward<Args>(args)...);
#endif
  }

  template <typename Func, typename... Args>
  int AsyncExecute(Func func, std::atomic<int> &finish, int &result, Args &&...args) {
    // Caller keeps result/finish alive until it observes completion with acquire.
    auto task = [&result, &finish, func, args...]() mutable {
    #ifdef MSG_FUNC_TIMING
      TimerAverage timer("msg_timing_func");
      timer.Start();
    #endif
      result = func(args...);
    #ifdef MSG_FUNC_TIMING
      timer.Stop();
    #endif
      finish.store(1, std::memory_order_release);
    };
    pool->enqueue(SimThreadInfo::dispatcher_thread_id, task);
    return DB::kOK;
  }

  ///
  /// Reads a record from the database.
  /// Field/value pairs from the result are stored in a vector.
  ///
  /// @param table The name of the table.
  /// @param key The key of the record to read.
  /// @param fields The list of fields to read, or NULL for all of them.
  /// @param result A vector of field/value pairs for the result.
  /// @return Zero on success, or a non-zero error code on error/record-miss.
  ///
  virtual int Read(const std::string &table, const std::string &key,
                   const std::vector<std::string> *fields,
                   std::vector<KVPair> &result) {
    std::cerr << "Error: Read(const std::string &table, const std::string &key,\
                   const std::vector<std::string> *fields,\
                   std::vector<KVPair> &result) not overridden in derived class."
              << std::endl;
    return -1;
  };

  virtual int Read(uint64_t key, uintptr_t &value) {
    return SyncExecute(
        [this](uint64_t k, uintptr_t &v) { return ReadInternal(k, v); }, key,
        value);
  };

  virtual int AsyncRead(uint64_t key, uintptr_t &value, std::atomic<int> &finish,
                        int &result) {
    return AsyncExecute(
        [this, &value](uint64_t k) { return ReadInternal(k, value); }, finish,
        result, key);
  }

  virtual int ReadInternal(uint64_t key, uintptr_t &value) {
    std::cerr
        << "Error: ReadInternal(uint64_t key) not overridden in derived class."
        << std::endl;
    return -1;
  }

  ///
  /// Performs a range scan for a set of records in the database.
  /// Field/value pairs from the result are stored in a vector.
  ///
  /// @param table The name of the table.
  /// @param key The key of the first record to read.
  /// @param record_count The number of records to read.
  /// @param fields The list of fields to read, or NULL for all of them.
  /// @param result A vector of vector, where each vector contains field/value
  ///        pairs for one record
  /// @return Zero on success, or a non-zero error code on error.
  ///
  virtual int Scan(const std::string &table, const std::string &key,
                   int record_count, const std::vector<std::string> *fields,
                   std::vector<std::vector<KVPair>> &result) {
    return -1;
  };

  virtual int ScanInternal(uint64_t key, int len,
    std::vector<std::vector<KVPair>>& result) {
    return -1;
  }

  virtual int AsyncScan(uint64_t key, int len, std::vector<std::vector<KVPair>>& scan_result, std::atomic<int> &finish,
                          int &result) {
    return AsyncExecute(
        [this](uint64_t key, int len, std::vector<std::vector<KVPair>>& scan_result) { return ScanInternal(key, len, scan_result); }, finish,
        result, key, len, scan_result);
  }

  ///
  /// Updates a record in the database.
  /// Field/value pairs in the specified vector are written to the record,
  /// overwriting any existing values with the same field names.
  ///
  /// @param table The name of the table.
  /// @param key The key of the record to write.
  /// @param values A vector of field/value pairs to update in the record.
  /// @return Zero on success, a non-zero error code on error.
  ///
  virtual int Update(const std::string &table, const std::string &key,
                     std::vector<KVPair> &values) {
    std::cerr
        << "Error: Update(const std::string &table, const std::string &key, "
           "std::vector<KVPair> &values) not overridden in derived class."
        << std::endl;
    return -1;
  };

  virtual int Update(uint64_t key, uintptr_t value) {
    return SyncExecute(
        [this](uint64_t k, uintptr_t v) { return UpdateInternal(k, v); }, key,
        value);
  };

  virtual int AsyncUpdate(uint64_t key, uintptr_t value, std::atomic<int> &finish,
                          int &result) {
    return AsyncExecute(
        [this](uint64_t k, uintptr_t v) { return UpdateInternal(k, v); }, finish,
        result, key, value);
  }

  virtual int UpdateInternal(uint64_t key, uintptr_t value) {
    std::cerr << "Error: UpdateInternal(uint64_t key) not overridden in "
                 "derived class."
              << std::endl;
    return -1;
  }

  ///
  /// Inserts a record into the database.
  /// Field/value pairs in the specified vector are written into the record.
  ///
  /// @param table The name of the table.
  /// @param key The key of the record to insert.
  /// @param values A vector of field/value pairs to insert in the record.
  /// @return Zero on success, a non-zero error code on error.
  ///
  virtual int Insert(const std::string &table, const std::string &key,
                     std::vector<KVPair> &values) {
    std::cerr
        << "Error: Insert(const std::string &table, const std::string &key, "
           "std::vector<KVPair> &values) not overridden in derived class."
        << std::endl;
    return -1;
  };

  virtual int Insert(uint64_t key, uintptr_t value) {
    return SyncExecute(
        [this](uint64_t k, uintptr_t v) { return InsertInternal(k, v); }, key,
        value);
  };

  virtual int AsyncInsert(uint64_t key, uintptr_t value, std::atomic<int> &finish,
                          int &result) {
    return AsyncExecute(
        [this](uint64_t k, uintptr_t v) { return InsertInternal(k, v); }, finish,
        result, key, value);
  }

  virtual int InsertInternal(uint64_t key, uintptr_t value) {
    std::cerr << "Error: InsertInternal(uint64_t key) not overridden in "
                 "derived class."
              << std::endl;
    return -1;
  }

  ///
  /// Deletes a record from the database.
  ///
  /// @param table The name of the table.
  /// @param key The key of the record to delete.
  /// @return Zero on success, a non-zero error code on error.
  ///
  virtual int Delete(const std::string &table, const std::string &key) {
    std::cerr << "Error: Delete(const std::string &table, const std::string "
                 "&key) not overridden in derived class."
              << std::endl;
    return -1;
  };

  virtual int Delete(uint64_t key) {
    return SyncExecute([this](uint64_t k) { return DeleteInternal(k); }, key);
  };

  virtual int AsyncDelete(uint64_t key, std::atomic<int> &finish, int &result) {
    return AsyncExecute([this](uint64_t k) { return DeleteInternal(k); },
                        finish, result, key);
  }

  virtual int DeleteInternal(uint64_t key) {
    std::cerr << "Error: DeleteInternal(uint64_t key) not overridden in "
                 "derived class."
              << std::endl;
    return -1;
  }

  void Poll(std::atomic<int> *poll_pos) {
#if defined(USE_MWAIT) && defined(__x86_64__) && defined(__WAITPKG__)
    _umonitor(poll_pos);
    while (*poll_pos != true) {
      _umwait(0, 1);
    }
#else
    while (*poll_pos != true);
#endif
  }

  virtual void InitStats() {}
  virtual void GetStats() {}
  virtual ~DB() {}

 public:
  ThreadPool *pool;
  int db_thread_id;
};

}  // namespace ycsbc

#endif  // YCSB_C_DB_H_
