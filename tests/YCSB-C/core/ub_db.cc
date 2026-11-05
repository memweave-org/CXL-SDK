#include "db/ub_db.h"
#ifdef YCSB_UB_TWO_HOST
#include "core/ub_session.h"
#include "db/ub_read_queue.h"
#include "shm/mm.h"
#include "utils/sim_id.h"
#include <atomic>
#include <cstdlib>
#include <future>
#include <thread>
#ifdef ENABLE_BWTREE_DB
#include "db/bwtree_db.h"
#endif
#ifdef ENABLE_CLEVEL_HASH_DB
#include "db/clevel_hash_db.h"
#endif
#ifdef ENABLE_SHERMAN_DB
#include "Tree.h"
#include <mutex>
#endif

namespace ycsbc {
namespace {
bool enabled(const char* key) {
  const char* value=std::getenv(key);
  return value && std::string(value)=="1";
}
template<class T, class... Args> T* shared_new(Args&&... args) {
  return new (shared_atomic_memory().malloc(sizeof(T))) T(std::forward<Args>(args)...);
}
#ifdef ENABLE_SHERMAN_DB
DSM* local_dsm=nullptr;
Tree* local_tree=nullptr;
std::once_flag sherman_attach;
class ShermanDB final : public DB {
  nt<uint64_t>* locks_;
  nt<void*>* roots_;
  void* sequences_;
  bool nc_nodes_, direct_reads_;
  void attach(bool create) {
    cxl_transport::configure_nc_nodes(nc_nodes_,direct_reads_);
    cxl_transport::configure_shared_sequences(sequences_);
    if(!create) { global_lock_table=locks_; root_ptr_list=roots_; }
    DSMConfig config;
    config.machineNR=1;
    // DSM IDs persist across the load and transaction thread lifetimes.
    config.cacheConfig.cacheSize=((SimThreadInfo::worker_thread_count+20)*12+1023)/1024;
    local_dsm=DSM::getInstance(config);
    local_dsm->set_process_id(SimThreadInfo::worker_machine_id);
    local_dsm->set_direct_nc_reads(direct_reads_);
    local_dsm->registerThread();
    local_tree=new Tree(local_dsm,0,create);
    std::cout << "SHERMAN_ACCESS nodes=" << (nc_nodes_ ? "NC" : "CACHE")
              << " reads=" << (direct_reads_ ? "direct-fields" : "page-copy") << std::endl;
    if(create) { locks_=global_lock_table; roots_=root_ptr_list; }
  }
 public:
  ShermanDB() {
    const char* access=std::getenv("YCSB_UB_SHERMAN_ACCESS");
    const std::string mode=access ? access : "cache";
    if(mode!="cache" && mode!="nc-direct" && mode!="nc-memcpy")
      throw std::runtime_error("Unknown Sherman access mode: "+mode);
    nc_nodes_=mode!="cache"; direct_reads_=mode=="nc-direct";
    sequences_=cxl_transport::allocate_atomic(cxl_transport::shared_sequences_bytes());
    std::memset(sequences_,0,cxl_transport::shared_sequences_bytes());
    std::call_once(sherman_attach,[&]{ attach(true); });
  }
  int ThreadInit() override {
    std::call_once(sherman_attach,[&]{ attach(false); });
    local_dsm->registerThread(); local_tree->register_thread(0);
    return SimThreadInfo::worker_thread_id;
  }
  int ReadInternal(uint64_t key,uintptr_t& value) override {
    if(!local_tree->search(key,value)) return kErrorNoData;
    if(nc_nodes_ && !direct_reads_) {
      thread_local std::vector<char> scratch;
      scratch.resize(VALUE_ADDR_SIZE);
      void* (*volatile copy)(void*, const void*, size_t)=&std::memcpy;
      copy(scratch.data(),reinterpret_cast<void*>(value),scratch.size());
      utils::AccessValueByAddress(reinterpret_cast<uintptr_t>(scratch.data()));
    } else utils::AccessValueByAddress(value);
    return kOK;
  }
  int InsertInternal(uint64_t key,uintptr_t value) override {
    local_tree->insert(key,value); return kOK;
  }
  int UpdateInternal(uint64_t key,uintptr_t value) override { return InsertInternal(key,value); }
};
#endif

std::thread rpc_server;
std::atomic<bool> stop_server{false};
nt<uint64_t>* completed=nullptr;
class UbDB final : public DB {
  DB* target_;
  UbReadQueue* requests_=nullptr;
  bool all_rpc_;
  nt<uint64_t> completed_{0};
 public:
  explicit UbDB(DB* target):target_(target),all_rpc_(enabled("YCSB_UB_RPC")) {
    completed=&completed_;
    if(!all_rpc_) return;
    requests_=shared_new<UbReadQueue>(ycsbc_ub::threads(1));
    stop_server.store(false);
    std::promise<void> ready;
    auto started=ready.get_future();
    rpc_server=std::thread([this, ready=std::move(ready)]() mutable {
      try {
        cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(ycsbc_ub::cpu_start(),&cpus);
        if(pthread_setaffinity_np(pthread_self(),sizeof(cpus),&cpus))
          throw std::runtime_error("Cannot bind UB RPC service CPU");
        SimThreadInfo::setup_worker_ids(0,SimThreadInfo::worker_thread_count);
        const int id=target_->ThreadInit(); target_->Init();
        ready.set_value();
        while(!stop_server.load()) {
          if(!requests_->serve_one([this](uint64_t key,uintptr_t& value) {
            const int result=target_->ReadInternal(key,value);
            completed_.fetch_add(1);
            return result;
          })) std::this_thread::yield();
        }
        target_->ThreadClose(id);
      } catch(...) {
        // Startup errors reach the constructor. Runtime errors terminate the
        // worker so peer.py reports failure and stops the other host promptly.
        try { ready.set_exception(std::current_exception()); } catch(...) { std::terminate(); }
      }
    });
    try { started.get(); } catch(...) { rpc_server.join(); throw; }
  }
  ~UbDB() override {
    stop_server.store(true);
    if(rpc_server.joinable()) rpc_server.join();
    target_->~DB(); shared_atomic_memory().free(target_);
    if(requests_) { requests_->~UbReadQueue(); shared_atomic_memory().free(requests_); }
    completed=nullptr;
  }
  int ThreadInit() override {
    return all_rpc_ && !SimThreadInfo::is_master() ? -1 : target_->ThreadInit();
  }
  void ThreadClose(int id) override { if(id>=0) target_->ThreadClose(id); }
  int ReadInternal(uint64_t key,uintptr_t& value) override {
    if(all_rpc_ && !SimThreadInfo::is_master()) {
      const int client=SimThreadInfo::worker_thread_id-ycsbc_ub::first_thread(1);
      return requests_->read(client,key,value);
    }
    return target_->ReadInternal(key,value);
  }
  int UpdateInternal(uint64_t key,uintptr_t value) override {
    if(!SimThreadInfo::is_master()) throw std::runtime_error("A is read-only: UPDATE is forbidden");
    return target_->UpdateInternal(key,value);
  }
  int InsertInternal(uint64_t key,uintptr_t value) override {
    if(!SimThreadInfo::is_master()) throw std::runtime_error("A is read-only: INSERT is forbidden");
    return target_->InsertInternal(key,value);
  }
};
}
uint64_t UbRpcCompleted() { return completed ? completed->load() : 0; }
const char* UbRpcTransport() { return enabled("YCSB_UB_RPC") ? "mpmc-shared-queue" : "none"; }
DB* CreateUbDB(const std::string& name,int threads) {
  DB* target=nullptr;
#ifdef ENABLE_BWTREE_DB
  if(name=="bwtree") target=shared_new<BwTreeDB>(threads+1);
#endif
#ifdef ENABLE_CLEVEL_HASH_DB
  if(name=="clevelhash") target=shared_new<CLevelHashDB>(threads+1);
#endif
#ifdef ENABLE_SHERMAN_DB
  if(name=="sherman") target=shared_new<ShermanDB>();
#endif
  if(!target) throw std::runtime_error("UB index not built: "+name);
  return shared_new<UbDB>(target);
}
}
#endif
