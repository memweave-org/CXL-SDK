#pragma once

#define LOCAL_NO_CC

#include "core/db.h"
#include "utils/config.h"
#include "shm/cxl_type.h"
#include "clevel_hash.hpp"

namespace ycsbc {

class CLevelHashDB : public DB {
public:
  // int Read(const std::string &table, const std::string &key,
  //          const std::vector<std::string> *fields, std::vector<KVPair>
  //          &result);
  // int Scan(const std::string &table, const std::string &key, int len,
  //          const std::vector<std::string> *fields,
  //          std::vector<std::vector<KVPair>> &result);
  // int Update(const std::string &table, const std::string &key,
  //            std::vector<KVPair> &values);
  // int Insert(const std::string &table, const std::string &key,
  //            std::vector<KVPair> &values);
  // int Delete(const std::string &table, const std::string &key);

  int ReadInternal(uint64_t key, uintptr_t &value) override;

  int UpdateInternal(uint64_t key, uintptr_t value) override;

  int InsertInternal(uint64_t key, uintptr_t value) override;

  int DeleteInternal(uint64_t key) override;

  [[deprecated("Use ReadInternal instead.")]]
  int Read(uint64_t key, uint64_t &value) override;

  [[deprecated("Use UpdateInternal instead.")]]
  int Update(uint64_t key, uint64_t value) override;

  [[deprecated("Use InsertInternal instead.")]]
  int Insert(uint64_t key, uint64_t value) override;

  [[deprecated("Use DeleteInternal instead.")]]
  int Delete(uint64_t key) override;

  // void Init() override;

  void Close() override;

  // void InitStats() override;

  // void GetStats() override;

  CLevelHashDB(int thread_num);

  ~CLevelHashDB();

  int ThreadInit() override;
  void ThreadClose(int thread_id) override;
  int PoolThreadInit() override;
  void PoolThreadClose(int thread_id) override;

  int allocate();
  void release(int id);

private:
// #ifdef NO_CC
#ifdef LOCAL_NO_CC
#ifndef YCSB_CLEVEL_HASH_POWER
#define YCSB_CLEVEL_HASH_POWER 20
#endif
  using clevel_hash_64 = clevel_hash<uint64_t, uint64_t, std::hash<uint64_t>, std::equal_to<uint64_t>, YCSB_CLEVEL_HASH_POWER>;
  clevel_hash_64 *level;
#else
  clevel_hash<uint64_t, uint64_t> *level;
#endif
  int thread_num;
  cxl_vector<std::atomic<uint8_t>> bits;
};

} // namespace ycsbc
