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
#include <memory>
#include <mutex>
#include <unordered_map>
#ifdef ENABLE_BWTREE_DB
#include "db/bwtree_db.h"
#endif
#ifdef ENABLE_CLEVEL_HASH_DB
#include "db/clevel_hash_db.h"
#endif
#ifdef ENABLE_SHERMAN_DB
#include "Tree.h"
#endif

namespace ycsbc {
namespace {
template<class T, class... Args> T* shared_new(Args&&... args) {
  return new (shared_atomic_memory().malloc(sizeof(T))) T(std::forward<Args>(args)...);
}
#ifdef ENABLE_SHERMAN_DB
struct ShermanState {
  nt<uint64_t>* locks=nullptr;
  nt<void*>* roots=nullptr;
  void* sequences=nullptr;
  bool nc_nodes=false, direct_reads=false;
};
DSM* local_dsm=nullptr;
Tree* local_tree=nullptr;
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
  ShermanDB(ShermanState& state, bool create) {
    if(!create) {
      locks_=state.locks; roots_=state.roots; sequences_=state.sequences;
      nc_nodes_=state.nc_nodes; direct_reads_=state.direct_reads;
      attach(false); return;
    }
    const char* access=std::getenv("YCSB_UB_SHERMAN_ACCESS");
    const std::string mode=access ? access : "cache";
    if(mode!="cache" && mode!="nc-direct" && mode!="nc-memcpy")
      throw std::runtime_error("Unknown Sherman access mode: "+mode);
    nc_nodes_=mode!="cache"; direct_reads_=mode=="nc-direct";
    // NC nodes use their own CopySequence prefix, never the hashed table.
    sequences_=nullptr;
    if(!nc_nodes_) {
      sequences_=cxl_transport::allocate_atomic(cxl_transport::shared_sequences_bytes());
      std::memset(sequences_,0,cxl_transport::shared_sequences_bytes());
    }
    attach(true);
    state={locks_,roots_,sequences_,nc_nodes_,direct_reads_};
    ycsbc_ub::stage("SHERMAN_CONTROL storage=local-dram shared_version_table_bytes="+
                    std::to_string(nc_nodes_ ? 0 : cxl_transport::shared_sequences_bytes()));
  }
  int ThreadInit() override {
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


enum class IndexKind { BwTree, ClevelHash, Sherman };
class LocalDB;
class UbDB final : public DB {
public:
  IndexKind kind;
  int threads;
  bool rpc;
  DB* shared_target=nullptr;
  UbReadQueue* inbound[2]{nullptr,nullptr};
#ifdef ENABLE_SHERMAN_DB
  ShermanState sherman;
#endif
  UbDB(IndexKind k,int t,bool r):kind(k),threads(t),rpc(r) {}
  ~UbDB() override;
  int ThreadInit() override { return LocalUbDB(this)->ThreadInit(); }
  void ThreadClose(int id) override { LocalUbDB(this)->ThreadClose(id); }
  int ReadInternal(uint64_t key,uintptr_t& value) override { return LocalUbDB(this)->ReadInternal(key,value); }
  int InsertInternal(uint64_t key,uintptr_t value) override { return LocalUbDB(this)->InsertInternal(key,value); }
  int UpdateInternal(uint64_t key,uintptr_t value) override { return LocalUbDB(this)->UpdateInternal(key,value); }
};

DB* create_index(IndexKind kind,int threads) {
#ifdef ENABLE_BWTREE_DB
  if(kind==IndexKind::BwTree) return shared_new<BwTreeDB>(threads+1);
#endif
#ifdef ENABLE_CLEVEL_HASH_DB
  if(kind==IndexKind::ClevelHash) return shared_new<CLevelHashDB>(threads+1);
#endif
  throw std::runtime_error("Requested UB index is not built");
}

// These objects are ordinary process-private objects. No server thread handle,
// completion counter, or pointer to a private index is published to the peer.
class LocalDB final : public DB {
  DB* target_;
  const bool rpc_, sherman_, owner_;
  const unsigned machine_;
  UbReadQueue *incoming_=nullptr, *outgoing_=nullptr;
  std::unique_ptr<RpcQueueRouter> outgoing_router_;
  std::unique_ptr<ThreadPool> service_;
  std::atomic<uint64_t> completed_{0};
  // Each worker owns its counters; collect only after the clients have joined.
  // Counting local accesses must not add a contended atomic to the index path.
  struct alignas(64) ClientCounts { uint64_t sent=0, local=0; };
  std::vector<ClientCounts> counts_;

  int execute(UbReadQueue::Operation operation,uint64_t key,uintptr_t& value) {
    PrivateIndexMemoryScope scope(rpc_ ? &cacheable : nullptr);
    switch(operation) {
      case UbReadQueue::Operation::Read: return target_->ReadInternal(key,value);
      case UbReadQueue::Operation::Update: return target_->UpdateInternal(key,value);
      case UbReadQueue::Operation::Insert: return target_->InsertInternal(key,value);
    }
    throw std::runtime_error("Invalid RPC operation");
  }
  int dispatch(UbReadQueue::Operation operation,uint64_t key,uintptr_t& value) {
    if(operation!=UbReadQueue::Operation::Read && machine_==1)
      throw std::runtime_error("A workload is read-only: mutation is forbidden");
    if(rpc_ && UbRpcOwner(key)!=machine_) {
      const int client=SimThreadInfo::worker_thread_id-ycsbc_ub::first_thread(machine_);
      const int result=outgoing_->request(client,operation,key,value,*outgoing_router_);
      ++counts_.at(client).sent;
      return result;
    }
    if(!rpc_) return execute(operation,key,value);
    const int client=SimThreadInfo::worker_thread_id-ycsbc_ub::first_thread(machine_);
    ++counts_.at(client).local;
    // The original lambda task remains strictly within this process.
    std::atomic<bool> finished{false};
    int result=DB::kErrorNoData;
    service_->enqueue(client,[&,operation,key] {
      result=execute(operation,key,value);
      finished.store(true,std::memory_order_release);
    });
    while(!finished.load(std::memory_order_acquire)) UbReadQueue::relax();
    return result;
  }
public:
  explicit LocalDB(UbDB& descriptor)
      : target_(nullptr),rpc_(descriptor.rpc),sherman_(descriptor.kind==IndexKind::Sherman),
        owner_(SimThreadInfo::is_master()),machine_(SimThreadInfo::worker_machine_id),
        counts_(rpc_ ? ycsbc_ub::threads(machine_) : 0) {
    if(rpc_) {
      PrivateIndexMemoryScope scope(&cacheable);
      target_=create_index(descriptor.kind,descriptor.threads+ycsbc_ub::threads(machine_));
      if(!ycsbc_ub::in_local_index(target_)) throw std::runtime_error("RPC index escaped local DRAM");
      incoming_=descriptor.inbound[machine_]; outgoing_=descriptor.inbound[1-machine_];
      outgoing_router_=std::make_unique<RpcQueueRouter>(outgoing_->buckets(),outgoing_->policy());
    } else if(sherman_) {
#ifdef ENABLE_SHERMAN_DB
      target_=new ShermanDB(descriptor.sherman,owner_);
#else
      throw std::runtime_error("Sherman is not built");
#endif
    } else target_=descriptor.shared_target;
    if(!rpc_) return;
    // Reuse the original MQ worker loop, bucket scheduling and local queues.
    // Only the remote task representation differs: slot IDs and value bytes.
    PrivateIndexMemoryScope scope(&cacheable);
    const size_t workers=ycsbc_ub::threads(machine_);
    const size_t buckets=std::max(workers,incoming_->buckets());
    service_=std::make_unique<ThreadPool>(workers,buckets,
      [this] { const int id=ThreadInit(); target_->Init(); return id; },
      [this](int id) { ThreadClose(id); },
      [this](size_t bucket) {
        return incoming_->serve_bucket(bucket,[this](UbReadQueue::Operation operation,uint64_t key,uintptr_t& value) {
          if(UbRpcOwner(key)!=machine_) throw std::runtime_error("RPC reached the wrong partition");
          if(operation!=UbReadQueue::Operation::Read) {
            if(machine_!=1) throw std::runtime_error("A sent an RPC mutation");
            void* copy=cacheable.malloc(VALUE_ADDR_SIZE);
            std::memcpy(copy,reinterpret_cast<const void*>(value),VALUE_ADDR_SIZE);
            value=reinterpret_cast<uintptr_t>(copy);
          }
          const int result=execute(operation,key,value);
          completed_.fetch_add(1,std::memory_order_relaxed);
          return result;
        });
      },
      [this](size_t worker) {
        SimThreadInfo::setup_worker_ids(machine_,SimThreadInfo::worker_thread_count+worker);
        cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(ycsbc_ub::rpc_worker_cpu(worker),&cpus);
        if(pthread_setaffinity_np(pthread_self(),sizeof(cpus),&cpus))
          throw std::runtime_error("Cannot bind original MQ service CPU");
      });
    ycsbc_ub::stage(std::string("RPC_PARTITION role=")+(owner_?"B":"A")+
      " index=local-dram owner=key%2 transport=original-mq-thread-pool value_transport=bytes workers="+
      std::to_string(workers)+" buckets="+std::to_string(buckets)+
      " queue_policy="+RpcQueueRouter::name(outgoing_->policy())+
      " outgoing_buckets="+std::to_string(outgoing_->buckets())+
      " outstanding_scope=sender-process outstanding_storage=local-dram max_inflight_per_client=1");
  }
  ~LocalDB() override {
    service_.reset();
    if(sherman_) delete target_;
    else if(rpc_ || owner_) {
      PrivateIndexMemoryScope scope(rpc_ ? &cacheable : nullptr);
      target_->~DB(); shared_atomic_memory().free(target_);
    }
  }
  int ThreadInit() override {
    PrivateIndexMemoryScope scope(rpc_ ? &cacheable : nullptr);
    return target_->ThreadInit();
  }
  void ThreadClose(int id) override {
    PrivateIndexMemoryScope scope(rpc_ ? &cacheable : nullptr);
    target_->ThreadClose(id);
  }
  // Prefill runs independently on both hosts, before any workload RPCs.
  int prefill(uint64_t key,uintptr_t value) {
    if(!rpc_ || UbRpcOwner(key)!=machine_) throw std::runtime_error("Invalid local partition prefill");
    PrivateIndexMemoryScope scope(&cacheable);
    return target_->InsertInternal(key,value);
  }
  int InsertInternal(uint64_t key,uintptr_t value) override { return dispatch(UbReadQueue::Operation::Insert,key,value); }
  int UpdateInternal(uint64_t key,uintptr_t value) override { return dispatch(UbReadQueue::Operation::Update,key,value); }
  int ReadInternal(uint64_t key,uintptr_t& value) override { return dispatch(UbReadQueue::Operation::Read,key,value); }
  uint64_t completed() const { return completed_.load(std::memory_order_relaxed); }
  uint64_t sent() const { uint64_t sum=0; for(const auto& c:counts_) sum+=c.sent; return sum; }
  uint64_t local() const { uint64_t sum=0; for(const auto& c:counts_) sum+=c.local; return sum; }
  const void* index_address() const { return target_; }
};
std::mutex local_dbs_mutex;
std::unordered_map<DB*,std::unique_ptr<LocalDB>> local_dbs;
UbDB::~UbDB() {
  { std::lock_guard<std::mutex> lock(local_dbs_mutex); local_dbs.erase(this); }
  for(auto* queue:inbound) if(queue) { queue->~UbReadQueue(); shared_atomic_memory().free(queue); }
}
}
DB* LocalUbDB(DB* descriptor) {
  std::lock_guard<std::mutex> lock(local_dbs_mutex);
  auto& local=local_dbs[descriptor];
  if(!local) local=std::make_unique<LocalDB>(*static_cast<UbDB*>(descriptor));
  return local.get();
}
int UbPrefillLocal(DB* local_db,uint64_t key,uintptr_t value) {
  return static_cast<LocalDB*>(local_db)->prefill(key,value);
}
void ReleaseLocalUbDBs() {
  std::lock_guard<std::mutex> lock(local_dbs_mutex);
  local_dbs.clear();
}
uint64_t UbRpcCompleted() {
  uint64_t count=0; for(auto& item:local_dbs) count+=item.second->completed(); return count;
}
uint64_t UbRpcSent() {
  uint64_t count=0; for(auto& item:local_dbs) count+=item.second->sent(); return count;
}
uint64_t UbRpcLocal() {
  uint64_t count=0; for(auto& item:local_dbs) count+=item.second->local(); return count;
}
const void* UbLocalIndexAddress(DB* descriptor) {
  return static_cast<LocalDB*>(LocalUbDB(descriptor))->index_address();
}
const char* UbRpcTransport() { return ycsbc_ub::rpc_mode() ? "original-mq-thread-pool" : "none"; }
DB* CreateUbDB(const std::string& name,int threads) {
  IndexKind kind;
  if(name=="bwtree") kind=IndexKind::BwTree;
  else if(name=="clevelhash") kind=IndexKind::ClevelHash;
  else if(name=="sherman") kind=IndexKind::Sherman;
  else throw std::runtime_error("UB index not built: "+name);
  const bool rpc=ycsbc_ub::rpc_mode();
#ifndef USE_MSG_QUEUE
  if(rpc) throw std::runtime_error("RPC requires build.sh rpc (cc_nocc_mq); rebuild the bundle");
#else
  if(!rpc) throw std::runtime_error("cc_nocc_mq requires RPC mode in a two-process run");
#endif
  if(rpc && kind==IndexKind::Sherman) throw std::runtime_error("Sherman uses shared-node access");
  auto* descriptor=shared_new<UbDB>(kind,threads,rpc);
  if(rpc) {
    const auto policy=RpcQueueRouter::configured_policy();
    for(unsigned receiver=0;receiver<2;++receiver) {
      const auto clients=ycsbc_ub::threads(1-receiver);
      const auto buckets=policy==RpcQueueRouter::Policy::Fixed ? clients : ycsbc_ub::threads(receiver);
      descriptor->inbound[receiver]=shared_new<UbReadQueue>(clients,256,VALUE_ADDR_SIZE,buckets,policy);
    }
  } else if(kind!=IndexKind::Sherman) descriptor->shared_target=create_index(kind,threads);
  LocalUbDB(descriptor);
  return descriptor;
}
}
#endif
