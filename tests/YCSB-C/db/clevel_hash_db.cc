#include "db/clevel_hash_db.h"

#include <cstdint>

namespace ycsbc {
CLevelHashDB::CLevelHashDB(int thread_num)
    : DB(),
      thread_num(thread_num), bits(thread_num) {
#ifndef LOCAL_NO_CC
  std::string clevel_path = "/dev/shm/cxl-clevel";
  std::string layout = "clevel_hash";
  if (std::filesystem::exists(clevel_path)) {
    std::filesystem::remove_all(clevel_path);
  }
  pop = nvobj::pool<root>::create(clevel_path, layout, 1024ul*1024*1024*4,
                                  S_IWUSR | S_IRUSR);
  auto proot = pop.root();

  nvobj::transaction::manual tx(pop);

  proot->cons = nvobj::make_persistent<persistent_map_type>();
  proot->cons->set_thread_num(thread_num);

  nvobj::transaction::commit();
#else
  level = new clevel_hash_64();
  if (level != nullptr) {
    level->set_thread_num(thread_num);
  }
#endif
  for (int i = 0; i < thread_num; ++i) {
    bits[i].store(false);
  }
#ifdef USE_MSG_QUEUE
  this->db_thread_id = this->ThreadInit();
  pool = new ThreadPool(
      thread_num - 1, 12, [this]() { return this->PoolThreadInit(); },
      [this](int thread_id) { this->PoolThreadClose(thread_id); });
#endif
}

CLevelHashDB::~CLevelHashDB() {
#ifdef LOCAL_NO_CC
  delete level;
#else
  pop.close();
#endif
}

int CLevelHashDB::ReadInternal(uint64_t key, uintptr_t &value) {
#ifdef LOCAL_NO_CC
  auto ret = level->search(key);
#else
  auto map = pop.root()->cons;
  auto ret = map->search(persistent_map_type::key_type(key));
#endif
  if (ret.found) {
    value = ret.value->second;
    utils::AccessValueByAddress(value);
    return DB::kOK;
  } else {
    return DB::kErrorNoData;
  }
}

int CLevelHashDB::UpdateInternal(uint64_t key, uintptr_t value) {
  std::pair<uint64_t, uintptr_t> pair(key, value);
#ifdef LOCAL_NO_CC
  level->update(pair, SimThreadInfo::worker_thread_id);
#else
  auto map = pop.root()->cons;
  map->update(persistent_map_type::value_type(pair),
              SimThreadInfo::worker_thread_id);
#endif
  return DB::kOK;
}

int CLevelHashDB::InsertInternal(uint64_t key, uintptr_t value) {
  std::pair<uint64_t, uintptr_t> pair(key, value);
#ifdef LOCAL_NO_CC
  level->insert(pair, SimThreadInfo::worker_thread_id, key);
#else
  auto map = pop.root()->cons;
  map->insert(persistent_map_type::value_type(pair),
              SimThreadInfo::worker_thread_id, key);
#endif
  return DB::kOK;
}

int CLevelHashDB::DeleteInternal(uint64_t key) {
#ifdef LOCAL_NO_CC
  auto ret = level->erase(key, SimThreadInfo::worker_thread_id);
#else
  auto map = pop.root()->cons;
  auto ret = map->erase(persistent_map_type::key_type(key),
                        SimThreadInfo::worker_thread_id);
#endif
  if (ret.found) {
    return DB::kOK;
  } else {
    return DB::kErrorNoData;
  }
}

void CLevelHashDB::Close() {
#ifdef USE_MSG_QUEUE
  if (pool) {
    pool->close();
  }
#endif
}

int CLevelHashDB::ThreadInit() {
  int thread_id = allocate();
#ifdef USE_MSG_QUEUE
  // Single dispatcher thread for this DB
  // SimThreadInfo::setup_dispatcher_nr(2);
  // SimThreadInfo::setup_dispatcher_id(0);
  return thread_id;
#else
  return thread_id;
#endif
}

void CLevelHashDB::ThreadClose(int thread_id) {
  release(thread_id);
}

int CLevelHashDB::PoolThreadInit() {
  int thread_id = allocate();
  return thread_id;
}

void CLevelHashDB::PoolThreadClose(int thread_id) {
  release(thread_id);
}

[[deprecated("Use ReadInternal instead.")]]
int CLevelHashDB::Read(uint64_t key, uint64_t &value) {
#ifdef USE_MSG_QUEUE
  std::atomic<int> finish{false};
  auto task = [this, key, &value, &finish]() {
    int ret = this->ReadInternal(key, value);
#ifdef RETURN_SYNC
    finish = true;
#endif
    return ret;
  };
#ifdef RETURN_SYNC
  pool->enqueue(utils::RandomValueNum(), task);
#if defined(USE_MWAIT) && defined(__x86_64__) && defined(__WAITPKG__)
  _umonitor(&finish);
  while (finish != true) {
    _umwait(0, 1);
  }
#else
  while (finish != true);
#endif
  return DB::kOK;
#else
  pool->enqueue(utils::RandomValueNum(), task);
  return DB::kOK;
#endif
#else
  return ReadInternal(key, value);
#endif
}

[[deprecated("Use UpdateInternal instead.")]]
int CLevelHashDB::Update(uint64_t key, uint64_t value) {
#ifdef USE_MSG_QUEUE
  std::atomic<int> finish{false};
  auto task = [this, key, value, &finish]() {
    int ret = this->UpdateInternal(key, value);
#ifdef RETURN_SYNC
    finish = true;
#endif
    return ret;
  };
#ifdef RETURN_SYNC
  pool->enqueue(utils::RandomValueNum(), task);
#if defined(USE_MWAIT) && defined(__x86_64__) && defined(__WAITPKG__)
  _umonitor(&finish);
  while (finish != true) {
    _umwait(0, 1);
  }
#else
  while (finish != true);
#endif
  return DB::kOK;
#else
  pool->enqueue(utils::RandomValueNum(), task);
  return DB::kOK;
#endif
#else
  return UpdateInternal(key, value);
#endif
}

[[deprecated("Use InsertInternal instead.")]]
int CLevelHashDB::Insert(uint64_t key, uint64_t value) {
#ifdef USE_MSG_QUEUE
  std::atomic<int> finish{false};
  auto task = [this, key, value, &finish]() {
    int ret = this->InsertInternal(key, value);
#ifdef RETURN_SYNC
    finish = true;
#endif
    return ret;
  };
#ifdef RETURN_SYNC
  pool->enqueue(utils::RandomValueNum(), task);
#if defined(USE_MWAIT) && defined(__x86_64__) && defined(__WAITPKG__)
  _umonitor(&finish);
  while (finish != true) {
    _umwait(0, 1);
  }
#else
  while (finish != true);
#endif
  return DB::kOK;
#else
  pool->enqueue(utils::RandomValueNum(), task);
  return DB::kOK;
#endif
#else
  return InsertInternal(key, value);
#endif
}

[[deprecated("Use DeleteInternal instead.")]]
int CLevelHashDB::Delete(uint64_t key) {
#ifdef USE_MSG_QUEUE
  std::atomic<int> finish{false};
  auto task = [this, key, &finish]() {
    int ret = this->DeleteInternal(key);
#ifdef RETURN_SYNC
    finish = true;
#endif
    return ret;
  };
#ifdef RETURN_SYNC
  pool->enqueue(utils::RandomValueNum(), task);
#if defined(USE_MWAIT) && defined(__x86_64__) && defined(__WAITPKG__)
  _umonitor(&finish);
  while (finish != true) {
    _umwait(0, 1);
  }
#else
  while (finish != true);
#endif
  return DB::kOK;
#else
  pool->enqueue(utils::RandomValueNum(), task);
  return DB::kOK;
#endif
#else
  return DeleteInternal(key);
#endif
}

int CLevelHashDB::allocate() {
  for (int i = 0; i < thread_num; ++i) {
    uint8_t expected = 0;
    if (bits[i].compare_exchange_strong(expected, 1)) {
      return i;
    }
  }
  fprintf(stderr, "[%s:%d][%s] thread %lx exit 1\n", __FILE__, __LINE__, __func__, pthread_self());
  exit(1);
  return -1;
}

void CLevelHashDB::release(int id) {
  if (id >= 0 && id < thread_num) {
    bits[id].store(0);
  } else {
    fprintf(stderr, "[%s:%d][%s] thread %lx exit 1\n", __FILE__, __LINE__, __func__, pthread_self());
    exit(1);
  }
}

}  // namespace ycsbc