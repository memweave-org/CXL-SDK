// Bounded, untimed functional assertions with the file-backed SDK fixture.
// No latency/throughput result. This does not emulate non-coherent CPU caches.
#include "core/ub_session.h"
#include "core/op_generator_int_key_addr.h"
#include "db/db_factory.h"
#include "db/ub_db.h"
#include "db/ub_read_queue.h"
#include "shm/ub.h"
#include "utils/sim_id.h"
#if defined(ENABLE_BWTREE_DB) && defined(OPT_IN_USE_FLAG) && BWTREE_PROCESS_MAPPING_CACHE
#include "bwtree.h"
#endif
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

namespace {
constexpr int clients=4, keys=64, queue_rounds=128;
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
void transfer(int fd,void* p,size_t n,bool writing) {
  auto* b=static_cast<char*>(p);
  while(n) {
    ssize_t count=writing ? ::write(fd,b,n) : ::read(fd,b,n);
    if(count<0 && errno==EINTR) continue;
    require(count>0,"contract peer disconnected"); n-=count;b+=count;
  }
}
void barrier(int in,int out) { char token=1;transfer(out,&token,1,true);transfer(in,&token,1,false); }
void generation_contract(utils::Properties props) {
  // Force every B operation to UPDATE: A must still produce only READs,
  // both with and without the suite switch that caused the regression.
  props.SetProperty("readproportion","0");props.SetProperty("updateproportion","1");
  for(bool suite:{false,true}) {
    if(suite) setenv("YCSB_UB_SUITE","1",1);else unsetenv("YCSB_UB_SUITE");
    ycsbc::CoreWorkload workload;workload.Init(props);
    ycsbc::OpGeneratorIntKeyAddr generator(&workload);
    auto operations=generator.GenerateOperations(128,1+clients);
    for (int group=0;group<1+clients;++group) {
      auto& shared=operations[group];
      using Operation=ycsbc::OpGeneratorIntKeyAddr::Operation;
      std::vector<Operation,ycsbc_ub::LocalDramAllocator<Operation>> local(shared.begin(),shared.end());
      ycsbc_ub::check_local_workload(local.data(),local.size()*sizeof(Operation),group,local.size());
      require(local.size()==shared.size(),"local trace count changed");
      for (size_t i=0;i<local.size();++i)
        require(local[i].type==shared[i].type && local[i].key==shared[i].key && local[i].value==shared[i].value,
                "local trace changed operation or index value address");
      if (!shared.empty()) {
        const auto key=shared[0].key;
        shared[0].key=key+1;
        require(local[0].key==key,"local trace aliases shared NC descriptors");
        shared[0].key=key;
      }
    }
    size_t reads=0,writes=0;
    for(int group=0;group<1+clients;++group) for(const auto& op:operations[group]) {
      require(op.type==(group==0?ycsbc::UPDATE:ycsbc::READ),"A generated a mutation");
      if(group==0) ++writes;else ++reads;
    }
    require(reads==64 && writes==64,"wrong per-host operation count");
  }
}
void queue_contract(bool owner,int in,int out) {
  ycsbc::UbReadQueue* queue=nullptr;
  if(owner) {
    queue=new(shared_atomic_memory().malloc(sizeof(*queue))) ycsbc::UbReadQueue(clients,2);
    for(const void* p:{static_cast<const void*>(queue),queue->request_storage(),queue->message_storage()})
      require(reinterpret_cast<uintptr_t>(p)>=0x400000000000ul && reinterpret_cast<uintptr_t>(p)<0x400008000000ul,"RPC storage is not in shared NC");
  }
  transfer(owner?out:in,&queue,sizeof(queue),owner);
  if(owner) {
    int completed=0;
    while(completed<clients*queue_rounds) {
      if(queue->serve_one([](uint64_t key,uintptr_t& value) { value=key^0x12345678;return int(key%3); })) ++completed;
      else std::this_thread::yield();
    }
    require(!queue->serve_one([](uint64_t,uintptr_t&){return -1;}),"queue contains a duplicate");
  } else {
    for(int bad:{-1,clients}) {
      bool rejected=false;uintptr_t value=0;
      try { queue->read(bad,0,value); } catch(const std::runtime_error&) { rejected=true; }
      require(rejected,"invalid client accepted");
    }
    std::vector<std::thread> threads;
    for(int c=0;c<clients;++c) threads.emplace_back([queue,c] {
      for(int i=0;i<queue_rounds;++i) {
        uint64_t key=uint64_t(c*queue_rounds+i);uintptr_t value=0;
        require(queue->read(c,key,value)==int(key%3) && value==(key^0x12345678),"stale/cross-client RPC response");
      }
    });
    for(auto& thread:threads) thread.join();
  }
  barrier(in,out);
  if(owner) queue->~UbReadQueue();
}
#if defined(ENABLE_BWTREE_DB) && defined(OPT_IN_USE_FLAG) && BWTREE_PROCESS_MAPPING_CACHE
void mapping_gc_contract(bool owner,int in,int out) {
  using Tree=wangziqi2013::bwtree::BwTree<long,long>;
  constexpr NodeID id=MAPPING_TABLE_SIZE-1;
  struct State { Tree* tree; const Tree::BaseNode* old; const Tree::BaseNode* replacement; } state{};
  if(owner) {
    state.tree=new(shared_atomic_memory().malloc(sizeof(Tree))) Tree(false,false);
    state.tree->UpdateThreadLocal(2);
    auto node=[] {
      return reinterpret_cast<Tree::InnerNode*>(Tree::ElasticNode<Tree::KeyNodeIDPair>::Get(
          0,Tree::NodeType::InnerType,0,0,std::make_pair(0L,NodeID{0}),std::make_pair(0L,NodeID{0})));
    };
    state.old=node(); state.replacement=node();
    state.tree->mapping_table[id].store(state.old);
  }
  transfer(owner?out:in,&state,sizeof(state),owner);
  auto* tree=state.tree;
  tree->AssignGCID(owner?0:1);
  tree->PrepareLocalMappingCache();
  require(tree->GetNode(id,false)==state.old,"initial inner cache fill");
  if(!owner) {
    // Hold a real tree slot busy while both GetNode entry points run on A.
    // Neither may expose the marker or wait for this gated filling thread.
    std::promise<void> acquired,resume;
    auto acquired_future=acquired.get_future(),resume_future=resume.get_future();
    std::thread filling([&] {
      tree->GetLocalCacheMappingTable()[id].fetch_and_fill([&] {
        acquired.set_value();
        resume_future.wait();
        return tree->mapping_table[id].load();
      },[](const Tree::BaseNode* node) { return Tree::EncodeCachedNode(node); });
    });
    acquired_future.wait();
    require(tree->GetNode(id,false)==state.old,"busy cached lookup did not bypass to NC");
    require(tree->GetNode(id,true)==state.old,"busy explicit bypass waited or returned a marker");
    resume.set_value();
    filling.join();
  }
  barrier(in,out);
  if(owner) {
    tree->mapping_table[id].store(state.replacement);
    tree->AddGarbageNode(state.old,id);
    tree->IncreaseEpoch(); // epoch 1
    tree->UpdateLastActiveEpoch();
  }
  barrier(in,out);
  if(!owner) {
    tree->UpdateLastActiveEpoch(); // A starts a newer operation using retained P.
    require(tree->GetNode(id,false)==state.old,"cache expired without a message");
  }
  barrier(in,out);
  if(owner) {
    tree->IncreaseEpoch(); // epoch 2, min 1: the original grace period ended.
    tree->PerformGC(0);
    auto* meta=tree->GetGCMetaData(0);
    require(meta->node_count==1,"reclaimed before invalidation ACK");
    auto* garbage=meta->header.next_p;
    require(garbage && garbage->cache_invalidation.sequence,"GC did not send a message");
    const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!tree->mapping_cache_invalidations.acknowledged(garbage->cache_invalidation)) {
      require(std::chrono::steady_clock::now()<limit,"GC message ACK timeout");
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    tree->PerformGC(0); // Record an epoch after all ACKs, even if already ACKed above.
    require(meta->node_count==1 && garbage->cache_clear_epoch==2,"missing post-ACK grace period");
    tree->UpdateLastActiveEpoch();
    tree->IncreaseEpoch(); // epoch 3, min 1: A still holds the cached old node.
    tree->PerformGC(0);
    require(meta->node_count==1,"freed while A still holds an old cached pointer");
  }
  barrier(in,out);
  if(!owner) {
    require(state.old->GetType()==Tree::NodeType::InnerType,"old node died inside read operation");
    require(tree->GetNode(id,false)==state.replacement,"message failed to refresh A's cache");
    tree->UpdateLastActiveEpoch();
    tree->UnregisterThread(1);
    bwtree_process_cache::shutdown();
  }
  barrier(in,out);
  if(owner) {
    const size_t live_before_gc=ub_arena_live(false);
    tree->UpdateLastActiveEpoch();
    tree->IncreaseEpoch(); // epoch 4, min 3: second grace period has ended.
    tree->PerformGC(0);
    require(tree->GetGCMetaData(0)->node_count==0,"GC did not resume after A quiesced");
    require(ub_arena_live(false)<live_before_gc,"GC did not return CACHE storage to the allocator");
    // The same-sized allocation must reuse the retired block, with no growth
    // of the CACHE high-water mark. This also exercises prepare()'s policy.
    const size_t high_water=ub_arena_used(false);
    const auto* reused=reinterpret_cast<Tree::InnerNode*>(Tree::ElasticNode<Tree::KeyNodeIDPair>::Get(
        0,Tree::NodeType::InnerType,0,0,std::make_pair(0L,NodeID{0}),std::make_pair(0L,NodeID{0})));
    require(reused==state.old,"retired BwTree CACHE address was not reused");
    require(ub_arena_used(false)==high_water,"reusing a retired node grew the CACHE arena");
    tree->epoch_manager.FreeEpochDeltaChain(reused);
    tree->mapping_table[id].store(nullptr);
    tree->epoch_manager.FreeEpochDeltaChain(state.replacement);
    tree->UnregisterThread(0);
    tree->~Tree();
    shared_atomic_memory().free(tree);
  }
}
#endif


void partitioned_rpc_contract(bool owner,const std::string& index,int in,int out) {
  ycsbc::DB* descriptor=nullptr;
  if(owner) {
    utils::Properties props; props.SetProperty("dbname",index);
    descriptor=ycsbc::DBFactory::CreateDB(props);
  }
  transfer(owner?out:in,&descriptor,sizeof(descriptor),owner);
  auto* db=ycsbc::LocalUbDB(descriptor);
  require(db!=descriptor,"RPC wrapper is still shared");
  require(ycsbc_ub::in_local_index(ycsbc::UbLocalIndexAddress(descriptor)),"RPC index is not in local DRAM");
  // After transport creation/attachment, index activity must not allocate NC.
  const size_t nc_before=ub_arena_used(true);
  int id=db->ThreadInit();
  // More than the fixture's initial Clevel capacity: exercise its resizer too.
  for(int k=1;k<=1024;++k) if(ycsbc::UbRpcOwner(k)==unsigned(owner?0:1)) {
    void* value=cacheable.malloc(64);std::memset(value,0x31,64);
    require(ycsbc_ub::in_local_index(value,64),"prefill value is not local");
    require(ycsbc::UbPrefillLocal(db,k,reinterpret_cast<uintptr_t>(value))==0,"partition prefill failed");
  }
  if(!owner) db->ThreadClose(id);
  barrier(in,out);
  if(!owner) for(int k:{1,2}) {
    bool update=false,insert=false;
    try { db->UpdateInternal(k,0); } catch(const std::runtime_error&) { update=true; }
    try { db->InsertInternal(k,0); } catch(const std::runtime_error&) { insert=true; }
    require(update && insert,"A workload mutation was accepted");
  }
  auto read_key=[&](int k,int phase) {
    uintptr_t value=0;
    require(db->ReadInternal(k,value)==0 && value,"partition read failed");
    const auto address=reinterpret_cast<uintptr_t>(value);
    require(!(address>=0x400000000000ul && address<0x400008000000ul),"RPC returned an NC slot");
    const auto* bytes=reinterpret_cast<const unsigned char*>(value);
    const unsigned char tag=bytes[0];
    require(phase==0 ? tag==0x31 : phase==2 ? tag==0x72 : (tag==0x31 || tag==0x72),"stale/wrong value bytes");
    for(int b=1;b<64;++b) require(bytes[b]==tag,"torn RPC value payload");
  };
  auto read_all=[&](int phase) {
    std::vector<std::thread> readers;
    for(int c=0;c<clients;++c) readers.emplace_back([&,c] {
      SimThreadInfo::setup_worker_ids(1,1+c);
      const int tid=db->ThreadInit();
      for(int k=1;k<=keys;++k) read_key(k,phase);
      db->ThreadClose(tid);
    });
    for(auto& thread:readers) thread.join();
  };
  if(owner) for(int k=1;k<=keys;++k) read_key(k,0);
  else read_all(0);
  barrier(in,out);
  if(owner) for(int k=1;k<=keys;++k) {
    void* value=cacheable.malloc(64);std::memset(value,0x72,64);
    require(db->UpdateInternal(k,reinterpret_cast<uintptr_t>(value))==0,"partition update failed");
    // A must own a copy, not retain B's private address or its NC request slot.
    if(ycsbc::UbRpcOwner(k)==1) std::memset(value,0xee,64);
  } else read_all(1);
  barrier(in,out);
  if(!owner) read_all(2);
  barrier(in,out);
  require(ub_arena_used(true)==nc_before,"RPC index/control activity allocated NC storage");
  require(ycsbc::UbRpcSent()==uint64_t(owner?keys:3*clients*keys/2),"wrong outbound partition count");
  require(ycsbc::UbRpcCompleted()==uint64_t(owner?3*clients*keys/2:keys),"wrong inbound partition count");
  require(ycsbc::UbRpcLocal()==uint64_t(owner?keys:3*clients*keys/2),"local request incorrectly sent over RPC");
  if(owner) db->ThreadClose(id);
  else ycsbc::ReleaseLocalUbDBs();
  barrier(in,out); // Stop A's server before B destroys either shared queue.
  if(owner) { descriptor->~DB();shared_atomic_memory().free(descriptor); }
}

struct Handles { ycsbc::DB* db; uintptr_t old_values,new_values; };
void index_contract(bool owner,bool rpc,const std::string& index,int in,int out) {
  Handles handles{};
  if(owner) {
    utils::Properties props;props.SetProperty("dbname",index);
    handles.db=ycsbc::DBFactory::CreateDB(props);
    handles.old_values=reinterpret_cast<uintptr_t>(cacheable.malloc(keys*64));
    handles.new_values=reinterpret_cast<uintptr_t>(cacheable.malloc(keys*64));
    std::memset(reinterpret_cast<void*>(handles.old_values),0x31,keys*64);
    std::memset(reinterpret_cast<void*>(handles.new_values),0x72,keys*64);
    clwb(reinterpret_cast<void*>(handles.old_values),keys*64);
    clwb(reinterpret_cast<void*>(handles.new_values),keys*64);
  }
  transfer(owner?out:in,&handles,sizeof(handles),owner);
  auto* db=handles.db;
  const bool replace_existing=std::getenv("YCSB_UB_SUITE")!=nullptr;
  int id=owner?db->ThreadInit():-1;
  if(owner) for(int k=0;k<keys;++k)
    require(db->InsertInternal(k+1,handles.old_values+k*64)==ycsbc::DB::kOK,"B insert failed");
  barrier(in,out);
  if(!owner) {
    bool update=false,insert=false;
    try { db->UpdateInternal(1,handles.new_values); } catch(const std::runtime_error&) { update=true; }
    try { db->InsertInternal(keys+1,handles.new_values); } catch(const std::runtime_error&) { insert=true; }
    require(update && insert,"A mutation reached the index or RPC");
  }
  auto read_all=[&](int phase) {
    std::vector<std::thread> threads;
    for(int c=0;c<clients;++c) threads.emplace_back([&,c] {
      SimThreadInfo::setup_worker_ids(1,1+c);
      const int tid=db->ThreadInit();
      for(int k=0;k<keys;++k) {
        uintptr_t value=0;
        require(db->ReadInternal(k+1,value)==ycsbc::DB::kOK,"read failed");
        const bool old=value==handles.old_values+k*64, fresh=value==handles.new_values+k*64;
        require((phase==0 || !replace_existing)?old:phase==2?fresh:(old||fresh),"wrong index value");
      }
      db->ThreadClose(tid);
    });
    for(auto& thread:threads) thread.join();
  };
  // B's main thread owns its index registration; A uses four client threads.
  if(owner) {
    for(int k=0;k<keys;++k) { uintptr_t v=0;require(db->ReadInternal(k+1,v)==0 && v==handles.old_values+k*64,"B initial read failed"); }
  } else read_all(0);
  barrier(in,out);
  if(owner) for(int k=0;k<keys;++k)
    require(db->UpdateInternal(k+1,handles.new_values+k*64)==0,"B update failed");
  else read_all(1);
  barrier(in,out);
  if(!owner) read_all(2);
#if defined(ENABLE_BWTREE_DB) && defined(OPT_IN_USE_FLAG) && BWTREE_PROCESS_MAPPING_CACHE
  if(!owner) bwtree_process_cache::shutdown();
#endif
  barrier(in,out);
  if(owner) {
    require(ycsbc::UbRpcCompleted()==(rpc?3*clients*keys:0),"wrong RPC completion count");
    db->ThreadClose(id);
    db->~DB();shared_atomic_memory().free(db);
  }
}
void run(bool owner,const std::string& mode,int in,int out) {
  const bool queue=mode=="queue",rpc=mode.find("+rpc")!=std::string::npos;
  const std::string index=queue?"bwtree":mode.substr(0,mode.find('+'));
  const std::string access=index=="sherman"?mode.substr(mode.find('+')+1):"cache";
  setenv("YCSB_UB_SUITE","1",1);setenv("YCSB_UB_RPC",rpc?"1":"0",1);
  setenv("YCSB_UB_A_WRITABLE","0",1);setenv("YCSB_UB_SHERMAN_ACCESS",access.c_str(),1);
  SimThreadInfo::setup_worker_machine_count(2);SimThreadInfo::setup_worker_nr(1+clients);
  SimThreadInfo::setup_worker_db_count(1);SimThreadInfo::setup_machine_ids(owner?0:1);
  SimThreadInfo::setup_worker_ids(owner?0:1,owner?0:1);
  const int control=dup(STDERR_FILENO);require(control>=3,"control fd");
  setenv("YCSB_UB_CONTROL_FD",std::to_string(control).c_str(),1);
  utils::Properties props;props.SetProperty("dbname",index);
  props.SetProperty("readproportion","0.9");props.SetProperty("updateproportion","0.1");
  char ready=1;
  if(!owner) transfer(in,&ready,1,false);
  ycsbc_ub::prepare(props);
  if(owner) transfer(out,&ready,1,true);
  VALUE_ADDR_SIZE=64;
  if(owner) generation_contract(props);
  if(mode=="bwtree+legacy") unsetenv("YCSB_UB_SUITE");
  if(mode=="bwtree+cache-gc") {
#if defined(ENABLE_BWTREE_DB) && defined(OPT_IN_USE_FLAG) && BWTREE_PROCESS_MAPPING_CACHE
    mapping_gc_contract(owner,in,out);
#else
    throw std::runtime_error("mapping cache GC contract requires BwTree OPT");
#endif
  } else if(queue) queue_contract(owner,in,out);
  else if(rpc) partitioned_rpc_contract(owner,index,in,out);
  else index_contract(owner,rpc,index,in,out);
  barrier(in,out);
  // A unmaps before B releases/destroys the shared objects.
  if(!owner) { ycsbc_ub::finish();transfer(out,&ready,1,true); }
  else { transfer(in,&ready,1,false);ycsbc_ub::finish(); }
  close(control);
}
}
int main(int argc,char** argv) {
  if(argc!=2 || !std::getenv("UBSM_TEST_DIRECTORY")) return 2;
  setenv("YCSB_UB_PROVIDER","fixture",1);setenv("YCSB_UB_SIZE_MB","128",1);
  setenv("YCSB_UB_NC_SIZE_MB","128",1);setenv("YCSB_UB_RECORDS","64",1);
  setenv("YCSB_UB_OPS","64",1);setenv("YCSB_UB_THREADS_A","4",1);setenv("YCSB_UB_THREADS_B","1",1);
  cpu_set_t allowed;require(sched_getaffinity(0,sizeof(allowed),&allowed)==0,"affinity");
  int cpu=0;while(cpu<CPU_SETSIZE && !CPU_ISSET(cpu,&allowed)) ++cpu;
  setenv("YCSB_UB_CPU",std::to_string(cpu).c_str(),1);
  const std::string name="readonly-rpc-contract-"+std::to_string(getpid());setenv("YCSB_UB_NAME",name.c_str(),1);
  int ab[2],ba[2];if(pipe(ab)||pipe(ba)) return 2;
  const pid_t child=fork();if(child<0) return 2;
  alarm(30); // Deadlock watchdog, not a performance threshold.
  try {
    if(child==0) { close(ab[1]);close(ba[0]);run(false,argv[1],ab[0],ba[1]);_exit(0); }
    close(ab[0]);close(ba[1]);run(true,argv[1],ba[0],ab[1]);
    int status=0;waitpid(child,&status,0);require(WIFEXITED(status)&&WEXITSTATUS(status)==0,"child failed");
    if(std::string(argv[1])=="bwtree+cache-gc")
      std::cout<<"PASS bwtree+cache-gc: busy-reader bypass, post-ACK reclamation and CACHE reuse verified\n";
    else std::cout<<"PASS "<<argv[1]<<": A read-only; routing, value bytes and memory placement verified\n";
  } catch(const std::exception& e) {
    std::cerr<<e.what()<<'\n';
    if(child>0) { kill(child,SIGKILL);waitpid(child,nullptr,0); }
    return 1;
  }
}
