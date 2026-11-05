// Bounded, untimed functional assertions with the file-backed SDK fixture.
// No latency/throughput result. This does not emulate non-coherent CPU caches.
#include "core/ub_session.h"
#include "core/op_generator_int_key_addr.h"
#include "db/db_factory.h"
#include "db/ub_db.h"
#include "db/ub_read_queue.h"
#include "shm/ub.h"
#include "utils/sim_id.h"
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
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
  if(queue) queue_contract(owner,in,out);else index_contract(owner,rpc,index,in,out);
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
    std::cout<<"PASS "<<argv[1]<<": A read-only, shared queue/values verified\n";
  } catch(const std::exception& e) {
    std::cerr<<e.what()<<'\n';
    if(child>0) { kill(child,SIGKILL);waitpid(child,nullptr,0); }
    return 1;
  }
}
