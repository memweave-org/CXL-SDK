#include "bwtree.h"
#include "arena.h"
#include "core/core_workload.h"
#include <ubs_mem.h>
#include <ubs_mem_def.h>
#include <sys/mman.h>
#include <sched.h>
#include <unistd.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cmath>
using Tree=wangziqi2013::bwtree::BwTree<uint64_t,uint64_t>;
constexpr uintptr_t BASE=0x300000000000;
constexpr uintptr_t NC_BASE=0x400000000000;
static uintptr_t arena_end;
struct Header { Tree* tree; uint64_t records; };
template<class F> static void check(const char* op,F call) {
  std::cerr<<"BEGIN "<<op<<std::endl;
  const int rc=call();
  std::cerr<<"END "<<op<<" rc="<<rc<<std::endl;
  if(rc) throw std::runtime_error(op);
}
#define SDK(op, call) check(op,[&] { return (call); })
static uint64_t value_for(uint64_t key) { return key^0x5a17b9c3d401f02eULL; }
static bool lookup(Tree* tree,uint64_t key) {
  std::vector<uint64_t> values;
  tree->GetValue(key,values);
  if(values.size()!=1 || values[0]<BASE || (values[0]>arena_end-8 || values[0]%8)) return false;
  auto* payload=reinterpret_cast<uint64_t*>(values[0]);
  clflush(payload,8);
  return __atomic_load_n(payload,__ATOMIC_SEQ_CST)==value_for(key);
}
static void insert(Tree* tree,uint64_t key) {
  auto* payload=static_cast<uint64_t*>(cacheable.malloc(8));
  *payload=value_for(key); clwb(payload,8);
  if(!tree->Insert(key,reinterpret_cast<uint64_t>(payload))) throw std::runtime_error("duplicate key");
}
struct Op { uint64_t key; bool write; };
struct Stats { uint64_t reads=0,writes=0,errors=0,last_write=0; };
int main(int argc,char** argv) {
  bool initialized=false,created=false,owned=false;
  void* address=nullptr; void* nc_address=nullptr; size_t bytes=0,nc_bytes=0; bool nc_created=false; std::string name,nc_name;
  int exit_code=1;
  try {
    if(argc!=12) throw std::runtime_error("worker A|B NAME PROVIDER SIZE_MB THREADS RECORDS OPS SPEC CPU_START NC_SIZE_MB THREAD_SLOTS");
    std::string role=argv[1]; name=argv[2]; nc_name=name+"-nc";
    if(role!="A" && role!="B") throw std::runtime_error("bad role");
    const bool owner=role=="B";
    const uint64_t mb=std::stoull(argv[4]), records=std::stoull(argv[6]), ops=std::stoull(argv[7]);
    const int threads=std::stoi(argv[5]), cpu_start=std::stoi(argv[9]);
    if(mb<128 || mb%4 || mb>SIZE_MAX/(1UL<<20) || records<2 || records>INT32_MAX || !ops || ops>INT64_MAX/4 || threads<1 || threads>192 || cpu_start<0 || cpu_start+threads>CPU_SETSIZE)
      throw std::runtime_error("invalid sizes, threads or CPUs");
    const uint64_t nc_mb=std::stoull(argv[10]);
    if(nc_mb<128 || nc_mb%4 || nc_mb>SIZE_MAX/(1UL<<20)) throw std::runtime_error("invalid NC size");
    nc_bytes=nc_mb<<20;
    if(nc_bytes>UINTPTR_MAX-NC_BASE || (mb<<20)>NC_BASE-BASE) throw std::runtime_error("arena overlap/overflow");
    const int slots=std::stoi(argv[11]);
    if(slots<2*threads+2 || slots>386 || slots%2) throw std::runtime_error("invalid shared thread slots");
    const int main_id=slots-2+(owner?1:0);
    bytes=mb<<20;
    if(bytes>UINTPTR_MAX-BASE) throw std::runtime_error("mapping overflow");
    arena_end=BASE+bytes;
    ubs_mem_provider_t provider{};
    if(!argv[3][0] || strlen(argv[3])>=sizeof(provider.host_name)) throw std::runtime_error("bad provider");
    strcpy(provider.host_name,argv[3]);
    provider.socket_id=provider.numa_id=provider.port_id=UINT32_MAX;
    ubsmem_options_t options{};
    SDK("attributes",ubsmem_init_attributes(&options));
    SDK("initialize",ubsmem_initialize(&options)); initialized=true;
    if(owner) {
      SDK("allocate CACHE",ubsmem_shmem_allocate_with_provider(&provider,name.c_str(),bytes,0660,UBSM_FLAG_CACHE)); created=true;
    }
    if(owner) {
      SDK("allocate NONCACHE",ubsmem_shmem_allocate_with_provider(&provider,nc_name.c_str(),nc_bytes,0660,UBSM_FLAG_NONCACHE|UBSM_FLAG_WR_DELAY_COMP)); nc_created=true;
    }
    SDK("map NC RW",ubsmem_shmem_map(reinterpret_cast<void*>(NC_BASE),nc_bytes,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED_NOREPLACE,nc_name.c_str(),0,&nc_address));
    if(nc_address!=reinterpret_cast<void*>(NC_BASE)) throw std::runtime_error("SDK did not honor NC fixed VA");
    configure_nc_arena(static_cast<char*>(nc_address)+4096,nc_bytes-4096,owner);
    const int prot=owner?PROT_READ|PROT_WRITE:PROT_READ;
    SDK("map",ubsmem_shmem_map(reinterpret_cast<void*>(BASE),bytes,prot,MAP_SHARED|MAP_FIXED_NOREPLACE,name.c_str(),0,&address));
    if(address!=reinterpret_cast<void*>(BASE)) throw std::runtime_error("SDK did not honor fixed VA");
    SDK("ownership",ubsmem_shmem_set_ownership(name.c_str(),address,bytes,prot)); owned=true;
    std::cerr<<"MAPPED role="<<role<<" pid="<<getpid()<<" address="<<address<<" prot="<<prot<<std::endl;
    std::cerr<<"NC_MAPPED role="<<role<<" address="<<nc_address<<" prot=3"<<std::endl;
    auto* header=static_cast<Header*>(nc_address);
    configure_arena(address,bytes);
    Tree* tree;
    SimThreadInfo::setup_machine_ids(owner?1:0);
    SimThreadInfo::setup_worker_machine_count(2);
    SimThreadInfo::setup_worker_nr(threads+1);
    SimThreadInfo::setup_worker_ids(owner?1:0,main_id);
    if(owner) {
      tree=new(shared_atomic_memory().malloc(sizeof(Tree))) Tree(false,false);
      tree->UpdateThreadLocal(slots); tree->AssignGCID(main_id);
      std::cerr<<"STAGE loading records="<<records<<std::endl;
      for(uint64_t key=1;key<=records;++key) {
        insert(tree,key);
        if(key%10000==0 || key==records) std::cerr<<"STAGE loaded="<<key<<"/"<<records<<std::endl;
      }
      header->tree=tree; header->records=records;
      clwb(address,arena_used());
    } else {
      ub_read_only_thread=true;
      if(header->records!=records) throw std::runtime_error("records mismatch");
      tree=header->tree;
      if(reinterpret_cast<uintptr_t>(tree)<NC_BASE+4096 || reinterpret_cast<uintptr_t>(tree)>NC_BASE+nc_bytes-sizeof(Tree)) throw std::runtime_error("invalid tree pointer");
      tree->AssignGCID(main_id);
    }
    // Pre-generate YCSB traces before READY, outside throughput timing.
    std::cerr<<"STAGE generating trace ops="<<ops<<" threads="<<threads<<std::endl;
    std::vector<std::vector<Op>> traces(threads);
    for(int t=0;t<threads;++t) {
      utils::Properties props; std::ifstream spec(argv[8]);
      if(!spec) throw std::runtime_error("missing workload");
      props.Load(spec);
      const double reads=std::stod(props.GetProperty("readproportion","0"));
      const double updates=std::stod(props.GetProperty("updateproportion","0"));
      if(reads<0 || updates<0 || !std::isfinite(reads+updates) || std::abs(reads+updates-1.0)>1e-9)
        throw std::runtime_error("readproportion + updateproportion must equal 1");
      for(const char* key:{"insertproportion","scanproportion","readmodifywriteproportion"})
        if(std::stod(props.GetProperty(key,"0"))!=0) throw std::runtime_error("unsupported operation proportion");
      props.SetProperty("recordcount",std::to_string(records));
      props.SetProperty("operationcount",std::to_string(ops));
      props.SetProperty("insertstart","0");
      if(!owner) { props.SetProperty("readproportion","1");props.SetProperty("updateproportion","0"); }
      ycsbc::CoreWorkload workload; workload.Init(props);
      uint64_t count=ops/threads+(uint64_t(t)<ops%threads);
      traces[t].reserve(count);
      for(uint64_t i=0;i<count;++i) {
        auto op=workload.NextOperation();
        if(op!=ycsbc::READ && op!=ycsbc::UPDATE) throw std::runtime_error("only read/update supported");
        bool write=owner && op==ycsbc::UPDATE;
        uint64_t key=workload.NextTransactionNumKey()+1;
        if(write) key=records+1+i*threads+t; // current BwTree YCSB UPDATE -> fresh insert
        traces[t].push_back({key,write});
      }
    }
    if(!lookup(tree,1) || !lookup(tree,records)) throw std::runtime_error("initial shared lookup failed");
    std::cout<<"UBYCSB READY"<<std::endl;
    std::string command;
    while(std::getline(std::cin,command)) {
      if(command=="run") {
        std::vector<Stats> stats(threads); std::vector<std::thread> workers;
        std::atomic<int> ready{0}; std::atomic<bool> go{false};
        for(int t=0;t<threads;++t) workers.emplace_back([&,t] {
          try {
            cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(cpu_start+t,&cpus);
            if(sched_setaffinity(0,sizeof(cpus),&cpus)) throw std::runtime_error("CPU affinity failed");
            ub_read_only_thread=!owner;
            SimThreadInfo::setup_worker_ids(owner?1:0,t);
            tree->AssignGCID(2*t+(owner?1:0));
          } catch(...) { ++stats[t].errors; }
          ++ready; while(!go.load()) std::this_thread::yield();
          if(stats[t].errors) return;
          try {
            for(const auto& op:traces[t]) {
              if(op.write) { insert(tree,op.key); ++stats[t].writes; stats[t].last_write=op.key; }
              else { if(!lookup(tree,op.key)) ++stats[t].errors; ++stats[t].reads; }
            }
          } catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; ++stats[t].errors; }
          tree->UnregisterThread(2*t+(owner?1:0));
        });
        while(ready.load()!=threads) std::this_thread::yield();
        auto begin=std::chrono::steady_clock::now(); go=true;
        for(auto& w:workers) w.join();
        double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        Stats total;
        for(auto s:stats) { total.reads+=s.reads;total.writes+=s.writes;total.errors+=s.errors; }
        std::cout<<"UBYCSB RESULT {\"role\":\""<<role<<"\",\"reads\":"<<total.reads<<",\"writes\":"<<total.writes
          <<",\"errors\":"<<total.errors<<",\"seconds\":"<<std::setprecision(12)<<seconds<<",\"witnesses\":[";
        for(int t=0;t<threads;++t) std::cout<<(t?",":"")<<stats[t].last_write;
        std::cout<<"],\"allocated_bytes\":"<<arena_used()<<",\"nc_allocated_bytes\":"<<nc_arena_used()<<",\"bwtree_opt\":"
#ifdef OPT_IN_USE_FLAG
          <<"true"
#else
          <<"false"
#endif
          <<"}"<<std::endl;
      } else if(command.rfind("verify ",0)==0) {
        std::istringstream keys(command.substr(7)); uint64_t key; bool ok=true;
        while(keys>>key) if(key && !lookup(tree,key)) ok=false;
        std::cout<<"UBYCSB "<<(ok?"VERIFIED":"BAD_VALUE")<<std::endl;
      } else if(command=="close" || (command=="release" && owner)) {
        tree->UnregisterThread(main_id);
        SDK("ownership NONE",ubsmem_shmem_set_ownership(name.c_str(),address,bytes,PROT_NONE)); owned=false;
        SDK("unmap",ubsmem_shmem_unmap(address,bytes));address=nullptr;
        SDK("unmap NC",ubsmem_shmem_unmap(nc_address,nc_bytes));nc_address=nullptr;
        if(command=="release") { SDK("deallocate",ubsmem_shmem_deallocate(name.c_str()));created=false; SDK("deallocate NC",ubsmem_shmem_deallocate(nc_name.c_str()));nc_created=false; }
        SDK("finalize",ubsmem_finalize());initialized=false;
        std::cout<<"UBYCSB CLOSED"<<std::endl;exit_code=0;break;
      } else throw std::runtime_error("unknown command");
    }
  } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<std::endl; }
  if(owned && address) std::cerr<<"cleanup ownership rc="<<ubsmem_shmem_set_ownership(name.c_str(),address,bytes,PROT_NONE)<<std::endl;
  if(address && address!=MAP_FAILED) { int rc=ubsmem_shmem_unmap(address,bytes);std::cerr<<"cleanup unmap rc="<<rc<<std::endl;if(!rc)address=nullptr; }
  if(nc_address) { int rc=ubsmem_shmem_unmap(nc_address,nc_bytes); std::cerr<<"cleanup NC unmap rc="<<rc<<std::endl; if(!rc)nc_address=nullptr; }
  if(initialized && !address && !nc_address) std::cerr<<"cleanup finalize rc="<<ubsmem_finalize()<<std::endl;
  if(nc_created) std::cerr<<"OBJECT_MAY_REMAIN "<<nc_name<<std::endl;
  if(created) std::cerr<<"OBJECT_MAY_REMAIN "<<name<<std::endl;
  // Do not destroy shared BwTree members after unmap.
  std::cout.flush();std::cerr.flush();_Exit(exit_code);
}
