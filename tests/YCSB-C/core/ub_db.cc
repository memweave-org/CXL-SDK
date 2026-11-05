#include "db/ub_db.h"
#ifdef YCSB_UB_TWO_HOST
#include "core/ub_session.h"
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

namespace ycsbc {
namespace {
bool enabled(const char* key) {
  const char* value=std::getenv(key);
  return value && std::string(value)=="1";
}
template<class T, class... Args> T* shared_new(Args&&... args) {
  return new (shared_atomic_memory().malloc(sizeof(T))) T(std::forward<Args>(args)...);
}

// One outstanding request per A client. Only NC addresses/numbers cross hosts;
// no std::function, stack reference, or process-private buffer crosses the queue.
struct alignas(64) Mailbox {
  nt<uint64_t> state{0}; // 0 idle, 1 request published, 2 reply published
  uint64_t operation=0,key=0,value=0;
  int result=DB::kErrorConflict;
};
std::thread rpc_server;
std::atomic<bool> stop_server{false};
nt<uint64_t>* completed=nullptr;
class UbDB final : public DB {
  DB* target_;
  Mailbox* mailboxes_;
  int clients_;
  bool all_rpc_,forward_writes_;
  nt<uint64_t> completed_{0};
  int request(uint64_t operation,uint64_t key,uintptr_t& value) {
    const int slot=SimThreadInfo::worker_thread_id-ycsbc_ub::first_thread(1);
    if(slot<0 || slot>=clients_) throw std::runtime_error("Invalid UB RPC client slot");
    auto& box=mailboxes_[slot];
    box.operation=operation; box.key=key; box.value=value;
    box.state.store(1,std::memory_order_release);
    while(box.state.load(std::memory_order_acquire)!=2) std::this_thread::yield();
    value=box.value; const int result=box.result;
    box.state.store(0,std::memory_order_release);
    return result;
  }
 public:
  explicit UbDB(DB* target):target_(target),clients_(ycsbc_ub::threads(1)),
      all_rpc_(enabled("YCSB_UB_RPC")),
      forward_writes_(all_rpc_ || !enabled("YCSB_UB_A_WRITABLE")) {
    mailboxes_=static_cast<Mailbox*>(shared_atomic_memory().malloc(sizeof(Mailbox)*clients_));
    for(int i=0;i<clients_;++i) new (&mailboxes_[i]) Mailbox;
    completed=&completed_;
    if(!forward_writes_) return;
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
          bool worked=false;
          for(int i=0;i<clients_;++i) {
            auto& box=mailboxes_[i];
            if(box.state.load(std::memory_order_acquire)!=1) continue;
            worked=true;
            uintptr_t value=box.value;
            if(box.operation==0) box.result=target_->ReadInternal(box.key,value);
            else if(box.operation==1) box.result=target_->UpdateInternal(box.key,value);
            else box.result=target_->InsertInternal(box.key,value);
            box.value=value; completed_.fetch_add(1);
            box.state.store(2,std::memory_order_release);
          }
          if(!worked) std::this_thread::yield();
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
    shared_atomic_memory().free(mailboxes_);
  }
  int ThreadInit() override {
    return all_rpc_ && !SimThreadInfo::is_master() ? -1 : target_->ThreadInit();
  }
  void ThreadClose(int id) override { if(id>=0) target_->ThreadClose(id); }
  int ReadInternal(uint64_t key,uintptr_t& value) override {
    if(all_rpc_ && !SimThreadInfo::is_master()) return request(0,key,value);
    return target_->ReadInternal(key,value);
  }
  int UpdateInternal(uint64_t key,uintptr_t value) override {
    if(forward_writes_ && !SimThreadInfo::is_master()) return request(1,key,value);
    return target_->UpdateInternal(key,value);
  }
  int InsertInternal(uint64_t key,uintptr_t value) override {
    if(forward_writes_ && !SimThreadInfo::is_master()) return request(2,key,value);
    return target_->InsertInternal(key,value);
  }
};
}
uint64_t UbRpcCompleted() { return completed ? completed->load() : 0; }
DB* CreateUbDB(const std::string& name,int threads) {
  DB* target=nullptr;
#ifdef ENABLE_BWTREE_DB
  if(name=="bwtree") target=shared_new<BwTreeDB>(threads+1);
#endif
#ifdef ENABLE_CLEVEL_HASH_DB
  if(name=="clevelhash") target=shared_new<CLevelHashDB>(threads+1);
#endif
  if(!target) throw std::runtime_error("UB index not built: "+name);
  return shared_new<UbDB>(target);
}
}
#endif
