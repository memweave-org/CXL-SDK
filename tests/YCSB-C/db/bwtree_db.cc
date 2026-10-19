#include "db/bwtree_db.h"

#include <numa.h>

#include <cassert>
#include <cstdlib>
#include <cstring>

#include "utils/cpu_dist.h"
#include "utils/timing.h"
#include "core/utils.h"
#include "utils/sim_id.h"
#include "utils/timing.h"
#include "db/utils.h"
#ifdef USE_CXL
#include "shm/mempool.h"
#endif


namespace ycsbc {
#ifdef TIMING_LOAD_BALANCE
int ThreadPool::pool_index = 0;
#endif

#ifdef USE_CXL
namespace {
std::atomic<bool> bwtree_gc_dispatcher_registered{false};

struct BwTreeRemoteGCRequest {
  TreeType* tree_p;
  const TreeType::BaseNode* node_p;
};

void HandleBwTreeRemoteGC(msg_node_t* node, int from) {
  (void)from;
  BwTreeRemoteGCRequest request{};
  memcpy(&request, node->content, sizeof(request));
  request.tree_p->epoch_manager.FreeEpochDeltaChain(request.node_p);
  free(node);
}
}  // namespace
#endif

BwTreeDB::BwTreeDB(int thread_num)
    : DB(), tree(GetEmptyTree(true)), thread_num(thread_num), bits(thread_num) {
#ifdef NT_SIM
  SimThreadInfo::setup_worker_db_id();
#endif
  if (tree != nullptr) {
#ifdef USE_CXL
    bool expected = false;
    if (SimThreadInfo::worker_machine_count > 1 &&
        bwtree_gc_dispatcher_registered.compare_exchange_strong(expected,
                                                                 true)) {
      add_dispatcher(BWTREE_GC, HandleBwTreeRemoteGC);
    }
#endif
    // DBFactory passes one extra slot that is not used by BwTree operations.
    // Including it in epoch tracking pins min_epoch at 0 and prevents reclaim.
    tree->UpdateThreadLocal(thread_num - 1);
    for (int i = 0; i < thread_num; ++i) {
      bits[i].store(false);
    }
#ifdef USE_MSG_QUEUE
    this->db_thread_id = this->ThreadInit();
    pool = new ThreadPool(
        thread_num - 1, SimThreadInfo::dispatcher_thread_count, [this]() { return this->PoolThreadInit(); },
        [this](int thread_id) { this->PoolThreadClose(thread_id); });
#endif
  }
}

BwTreeDB::~BwTreeDB() {
#ifdef USE_MSG_QUEUE
  // Only the message-queue variant registers a DB-owned thread in the
  // constructor.  In direct-call variants db_thread_id is never initialized;
  // worker threads register and unregister themselves through ThreadInit() /
  // ThreadClose().
  this->ThreadClose(this->db_thread_id);
#endif
  tree->UpdateThreadLocal(1);
  DestroyTree(tree, true);
}

void BwTreeDB::Init() {}

void BwTreeDB::Close() {
#ifdef USE_MSG_QUEUE
  pool->close();
#endif
}

int BwTreeDB::PoolThreadInit() {
  // int thread_id = allocate();
  int thread_id = SimThreadInfo::worker_thread_id;
  // fprintf(stderr, "BwTreeDB::PoolThreadInit: thread_id=%d\n", thread_id);
  tree->AssignGCID(thread_id);
  return thread_id;
}

void BwTreeDB::PoolThreadClose(int thread_id) {
  tree->UnregisterThread(thread_id);
  // release(thread_id);
}

int BwTreeDB::ThreadInit() {
  // int thread_id = allocate();
  int thread_id = SimThreadInfo::worker_thread_id;
  tree->AssignGCID(thread_id);
  return thread_id;
}

void BwTreeDB::ThreadClose(int thread_id) {
  tree->UnregisterThread(thread_id);
  // release(thread_id);
}

[[deprecated("Use ReadInternal instead.")]]
int BwTreeDB::Read(const std::string& table, const std::string& key,
                   const std::vector<std::string>* fields,
                   std::vector<KVPair>& result) {
  std::atomic<int> finish{false};
  auto task = [this, key, &finish]() {
    long key_index = convert_std_hash(key);
    std::vector<long> val;
    tree->GetValue(key_index, val);
    auto ret = (*val.begin() > 0) ? DB::kOK : DB::kErrorNoData;
#ifdef RETURN_SYNC
    finish = true;
#endif
    return ret;
  };
#ifdef USE_MSG_QUEUE
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
    return task();
#endif
}

int BwTreeDB::ReadInternal(uint64_t key, uintptr_t &value) {
#ifdef TRX_TYPE_STAT
  read_cnt.fetch_add(1);
#endif
  std::vector<long> val;
  tree->GetValue(key, val);
  if (val.size() == 0) {
    return DB::kErrorNoData;
  }
  value = val[0];
  // access the real value by address
  utils::AccessValueByAddress(value);
  return DB::kOK;
}

int BwTreeDB::ScanInternal(uint64_t key, int len,
  std::vector<std::vector<KVPair>>& result) {
  auto element_iterator = tree->Begin(key);
  for (int i = 0; i < len; i++) {
    if (element_iterator.IsEnd()) {
      break;
    }
    [[maybe_unused]] auto& element = *element_iterator;
    result.emplace_back();
    element_iterator++;
  }
  return DB::kOK;
}

[[deprecated("Use ScanInternal instead.")]]
int BwTreeDB::Scan(const std::string& table, const std::string& key, int len,
                   const std::vector<std::string>* fields,
                   std::vector<std::vector<KVPair>>& result) {
#ifdef USE_MSG_QUEUE
  throw "Scan: function not implemented!";

// #ifdef RETURN_SYNC
//   int return_value = 0xF980AB;
//   pool->enqueue(RandomThreadNum() % thread_num, SCAN, tree, key,
//   &return_value);
// #if defined(USE_MWAIT) && defined(__x86_64__) && defined(__WAITPKG__)
//   _umonitor(&return_value);
//   while (return_value == 0xF980AB) {
//     _umwait(1, 0);
//   }
// #else
//   while (return_value == 0xF980AB);
// #endif
//   auto task = [this, key]() {
//     for (int i = 0; i < len; i++) {
//       if (element_iterator.IsEnd()) {
//         break;
//       }
//       auto& element = *element_iterator;
//       if (element.second != element.first + 1) {
//         printf("Scan Error: %ld %ld\n", element.first + 1, element.second);
//       }
//       element_iterator++;
//     }
//     return DB::kOK;
//   };
//   auto future = pool->enqueue(RandomThreadNum() % thread_num, task);
//   future_queue.push(future);
// #else
// #endif
#else
  long key_index = convert_std_hash(key);
  auto element_iterator = tree->Begin(key_index);
  for (int i = 0; i < len; i++) {
    if (element_iterator.IsEnd()) {
      break;
    }
    auto& element = *element_iterator;
    if (element.second != element.first + 1) {
      std::cerr << "Read Error: " << element.second << " " << element.first + 1
                << std::endl;
      return DB::kErrorNoData;
    }
    result.emplace_back();
    element_iterator++;
  }
#endif
  return DB::kOK;
}

[[deprecated("Use UpdateInternal instead.")]]
int BwTreeDB::Update(const std::string& table, const std::string& key,
                     std::vector<KVPair>& values) {
  std::atomic<int> finish{false};
  auto task = [this, key, &finish]() {
    long key_index = convert_std_hash(key);
    // long rand_key = utils::RandomValueNum();    
    auto ret = tree->Insert(key_index, key_index + 1) ? DB::kOK : DB::kErrorNoData;
    #ifdef RETURN_SYNC
    finish = true;
    #endif
    return ret;
  };
  #ifdef USE_MSG_QUEUE
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
    return task();
  #endif
}

int BwTreeDB::UpdateInternal(uint64_t key, uintptr_t value) {
#ifdef TRX_TYPE_STAT
  update_cnt.fetch_add(1);
#endif
  // TODO: Update with the real key is extremely slow, checking the reason
  auto ret = tree->Insert(utils::Random64(), value) ? DB::kOK : DB::kErrorNoData;
  return ret;
}

[[deprecated("Use InsertInternal instead.")]]
int BwTreeDB::Insert(const std::string& table, const std::string& key,
                     std::vector<KVPair>& values) {
  std::atomic<int> finish{false};
  auto task = [this, key, &finish]() {
    long key_index = convert_std_hash(key);
    auto ret = tree->Insert(key_index, key_index + 1) ? DB::kOK : DB::kErrorNoData;
    #ifdef RETURN_SYNC
    finish = true;
    #endif
    return ret;
  };
  #ifdef USE_MSG_QUEUE
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
    return task();
  #endif
}

int BwTreeDB::InsertInternal(uint64_t key, uintptr_t value) {
#ifdef TRX_TYPE_STAT
  insert_cnt.fetch_add(1);
#endif
  auto ret = tree->Insert(key, value) ? DB::kOK : DB::kErrorNoData;
  return ret;
}

[[deprecated("Use DeleteInternal instead.")]]
int BwTreeDB::Delete(const std::string& table, const std::string& key) {
  std::atomic<int> finish{false};
  auto task = [this, key, &finish]() {
    long key_index = convert_std_hash(key);
    auto ret = tree->Delete(key_index, key_index + 1);
    #ifdef RETURN_SYNC
    finish = true;
    #endif
    return ret;
  };
  #ifdef USE_MSG_QUEUE
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
    return task();
  #endif
}

int BwTreeDB::DeleteInternal(uint64_t key) {
  auto ret = tree->Delete(key, key);
  return ret;
}

void BwTreeDB::InitStats() {
  read_cnt.store(0);
  update_cnt.store(0);
  insert_cnt.store(0);
  // tree->total_count = 0;
  // tree->total_read = 0;
}

void BwTreeDB::GetStats() {
  std::cerr << "Read Count: " << read_cnt.load() << " ";
  std::cerr << "Update Count: " << update_cnt.load() << " ";
  std::cerr << "Insert Count: " << insert_cnt.load() << std::endl;
  tree->PrintGCStats(std::cerr);
  // std::cerr << "Average GetNode: " << tree->GetTime();
  // tree->reportTimers();
}

int BwTreeDB::allocate() {
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

void BwTreeDB::release(int id) {
  if (id >= 0 && id < thread_num) {
    bits[id].store(0);
  } else {
    fprintf(stderr, "[%s:%d][%s] thread %lx exit 1\n", __FILE__, __LINE__, __func__, pthread_self());
    exit(1);
  }
}

}  // namespace ycsbc
