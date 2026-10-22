#include "utils/platform.h"
#include "index.h"
#include "shm/mm.h"
#include "utils/sim_id.h"
#include "utils/atomic_variable.h"
#include "core/core_workload.h"
#ifdef TWO_PROCESS_RPC
#include "msg/mpmc_queue.h"
#endif
#include "core/db_config.h"
#include "../../ds/cxl_transport/memory.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <stdexcept>

constexpr size_t header_bytes=8ul<<20;
constexpr uintptr_t mapping_address=0x400000000000;
constexpr uintptr_t atomic_address=0x500000000000;
constexpr int max_threads=192;
struct alignas(64) Mailbox {
  nt<uint64_t> response_sequence{0};
  alignas(64) uint64_t key{}, value{}, write{}, sequence{};
  alignas(64) uint64_t ok{};
};
struct Shared {
  nt<uint64_t> initialized{0}, ready[2], done[2], validated[2], start{0};
  Handles handles;
  uint64_t* ub_cas_probe{};
#ifdef TWO_PROCESS_RPC
  MPMCQueue<uint32_t>* requests{};
#endif
  uint64_t pid[2]{}, completed[2]{}, errors[2]{};
  uint64_t witness[2][max_threads]{};
  double seconds[2]{};
  Mailbox mail[max_threads];
};
static_assert(sizeof(Shared)<(1ul<<20));
class Mapper: public CXLSystemMemoryMmapper {
  bool ub;
  UbMappingMode mode;
public:
  Mapper(const char* path,int id,bool u,UbMappingMode m=UbMappingMode::Legacy):CXLSystemMemoryMmapper(),ub(u),mode(m) {
    machine_no=id; num_machines=2;
    if(strlen(path)>=sizeof(this->path)) throw std::runtime_error("mapping path too long");
    strcpy(this->path,path);
    if(!ub) { fd=open(path,O_RDWR); if(fd<0) throw std::runtime_error("open backing file/device failed"); }
  }
  void allocate(void* b,size_t n) override {
    mmap_size=n;
    mmap_base=ub?map_ub_memory(path,b,n,mode):mmap(b,n,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED_NOREPLACE,fd,0);
    if(mmap_base==MAP_FAILED || mmap_base!=b) throw std::runtime_error("fixed shared mapping failed");
    per_machine_size=((n-header_bytes)/2)&~size_t((2ul<<20)-1);
    per_machine_base=(char*)mmap_base+header_bytes+machine_no*per_machine_size;
  }
  void close_ub() {
    if (ub && mmap_base) { unmap_ub_memory(mmap_base, mmap_size); mmap_base=nullptr; }
  }
  ~Mapper() override {
    // The benchmark explicitly closes after both workers stop all access.
    // Do not throw or infer peer quiescence from destruction on an error path.
  }
};
void wait_for(nt<uint64_t>& x,uint64_t value) {
  auto until=std::chrono::steady_clock::now()+std::chrono::minutes(20);
  while(x.load()!=value) {
    if(std::chrono::steady_clock::now()>until) throw std::runtime_error("peer barrier timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
void publish(void* p,size_t n) {
  clflush(p,n);
  shm_platform::full_fence();
}
uint64_t value_for(uint64_t k) { return k^0x5a17b9c3d401f02eull; }
uint64_t allocate_value(uint64_t key) {
  auto* p=(uint64_t*)cacheable.malloc(8); if(!p) throw std::bad_alloc();
  *p=value_for(key); publish(p,8); return (uint64_t)p;
}
bool get_checked(uint64_t key) {
  uint64_t v=0;
  if(!index_get(key,v) || v<mapping_address || v>=mapping_address+(1ull<<40)) return false;
#ifdef SHM_UB_SPLIT
  clflush((void*)v,8);
#endif
  return *(volatile uint64_t*)v==value_for(key);
}
struct Op {uint64_t key; bool write;};
int main(int argc,char** argv) try {
  // Intentionally explicit worker arguments; the Python runner validates them.
  if(argc!=11 && argc!=12) throw std::runtime_error("usage: bench ID THREADS RECORDS OPS SIZE_MB PATH file|ub SPEC RUN_TOKEN original|fresh-insert");
  const int id=std::stoi(argv[1]);
  std::string thread_arg=argv[2]; auto comma=thread_arg.find(',');
  if(comma==std::string::npos) throw std::runtime_error("THREADS must be N0,N1");
  const int counts[2]={std::stoi(thread_arg.substr(0,comma)),std::stoi(thread_arg.substr(comma+1))};
  if(id<0 || id>1) throw std::runtime_error("invalid process ID");
  const int threads=counts[id], total_threads=counts[0]+counts[1], thread_offset=id?counts[0]:0;
  const uint64_t records=std::stoull(argv[3]), ops=std::stoull(argv[4]);
  const size_t bytes=std::stoull(argv[5])<<20;
  const uint64_t token=std::stoull(argv[9]);
  const bool fresh=std::string(argv[10])=="fresh-insert" || std::string(BENCH_SYSTEM)=="bwtree" || std::string(BENCH_SYSTEM)=="rpc";
  auto write_key=[&](uint64_t key) {
    if(fresh) index_put(key,allocate_value(key));
    else index_update(key,allocate_value(key));
  };
  if(id<0 || id>1 || counts[0]<1 || counts[0]>max_threads || counts[1]<1 || counts[1]>max_threads || records<2 || !ops || bytes<(128ul<<20)) throw std::runtime_error("invalid worker arguments");
  set_value_size(8);
  SimThreadInfo::setup_machine_ids(id); SimThreadInfo::setup_worker_machine_count(2);
  SimThreadInfo::setup_worker_nr(total_threads+1); SimThreadInfo::setup_dispatcher_nr(total_threads);
#ifdef SHM_UB_SPLIT
  if (std::string(argv[7]) != "ub") throw std::runtime_error("split binary requires UB");
  const size_t nc_bytes = (argc == 12 ? std::stoull(argv[11]) : 512) << 20;
  if (nc_bytes < (128ul<<20) || nc_bytes % (4ul<<20)) throw std::runtime_error("invalid NC arena size");
  auto* nc_mapper = new Mapper((std::string(argv[6])+"-nc").c_str(),id,true,UbMappingMode::Noncacheable);
  nc_mapper->allocate((void*)atomic_address,nc_bytes);
  ub_configure_arena(true,nc_mapper->per_machine_base,nc_mapper->per_machine_size);
  auto* mapper=new Mapper(argv[6],id,true,UbMappingMode::Cacheable);
  mapper->allocate((void*)mapping_address,bytes);
  ub_configure_arena(false,mapper->per_machine_base,mapper->per_machine_size);
  cacheable=MemoryManager(false);
  auto* shared=(Shared*)atomic_address;
  std::cout << "UB_LAYOUT split data=" << (void*)mapping_address << " bytes=" << bytes
            << " atomic=" << (void*)atomic_address << " atomic_bytes=" << nc_bytes << std::endl;
#else
  auto* mapper=new Mapper(argv[6],id,std::string(argv[7])=="ub");
  cacheable=MemoryManager(mapper,(void*)mapping_address,bytes);
  cacheable.require_shared_allocations(true);
  auto* shared=(Shared*)mapping_address;
#endif
  auto* sequences=(char*)shared+(1ul<<20);
  if(!id) { new (shared) Shared(); memset(sequences,0,cxl_transport::shared_sequences_bytes()); }
  cxl_transport::configure_shared_sequences(sequences);
  if(!id) {
    if (std::string(argv[7]) == "ub") {
      shared->ub_cas_probe = static_cast<uint64_t*>(shared_atomic_memory().malloc(sizeof(uint64_t)));
      __atomic_store_n(shared->ub_cas_probe, 0, __ATOMIC_SEQ_CST);
    }
    index_open(shared->handles,true,total_threads);
#ifdef TWO_PROCESS_RPC
    shared->requests=new (shared_atomic_memory().malloc(sizeof(MPMCQueue<uint32_t>))) MPMCQueue<uint32_t>(4096);
#endif
    SimThreadInfo::setup_worker_ids(0,total_threads); index_thread(total_threads);
    for(uint64_t k=1;k<=records;++k) index_put(k,allocate_value(k));
    index_finish(total_threads);
    // Publish initialized nodes, roots, replica metadata and copy sequences.
#ifdef SHM_UB_SPLIT
    clwb(mapper->per_machine_base, ub_arena_used(false));
#else
    publish((void*)mapping_address,header_bytes+mapper->per_machine_size);
#endif
    shared->initialized.store(token);
  } else {
    wait_for(shared->initialized,token);
    index_open(shared->handles,false,total_threads);
  }
  if (std::string(argv[7]) == "ub") {
    // Both processes CAS the same word allocated from the mapped UB arena.
    auto* probe = shared->ub_cas_probe;
    const auto addr = reinterpret_cast<uintptr_t>(probe);
#ifdef SHM_UB_SPLIT
    if (addr < atomic_address + header_bytes || addr > atomic_address + nc_bytes - sizeof(*probe))
#else
    if (addr < mapping_address + header_bytes || addr > mapping_address + bytes - sizeof(*probe))
#endif
      throw std::runtime_error("CAS probe is outside the UB arena");
    uint64_t expected = id;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!__atomic_compare_exchange_n(probe, &expected, id + 1, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
      expected = id;
      if (std::chrono::steady_clock::now() > deadline)
        throw std::runtime_error("shared UB CAS probe timed out");
      std::this_thread::yield();
    }
    while (__atomic_load_n(probe, __ATOMIC_SEQ_CST) != 2) {
      if (std::chrono::steady_clock::now() > deadline)
        throw std::runtime_error("shared UB CAS probe peer timed out");
      std::this_thread::yield();
    }
    std::cout << "UB_CAS_PROBE id=" << id << " address=" << probe
              << " value=2 simulated_latency=off" << std::endl;
  }
  shared->pid[id]=getpid(); publish(&shared->pid[id],sizeof(uint64_t));
  std::cout<<"PROCESS id="<<id<<" pid="<<getpid()<<" threads="<<threads<<" shared="<<(void*)shared<<" backend="<<argv[7]<<std::endl;
  std::atomic<uint64_t> errors{0}, completed{0};
  std::atomic<int> local_ready{0};
  std::vector<std::thread> workers;
#ifdef TWO_PROCESS_RPC
  std::atomic<bool> stop_servers{false};
  std::vector<std::thread> servers;
  if(!id) for(int t=0;t<threads;++t) servers.emplace_back([&,t] {
    SimThreadInfo::setup_worker_ids(0,t); index_thread(t);
    while(!stop_servers.load()) {
      uint32_t who;
      if(!shared->requests->try_dequeue(who)) { shm_platform::pause(); continue; }
      auto& m=shared->mail[who];
      clflush(&m.key,sizeof(m.key)); shm_platform::full_fence();
      if(m.write) write_key(m.key);
      m.ok=m.write || get_checked(m.key); publish(&m.ok,sizeof(m.ok)); m.response_sequence.store(m.sequence);
    }
    index_finish(t);
  });
#endif
  for(int t=0;t<threads;++t) workers.emplace_back([&,t] {
    const int tid=thread_offset+t;
    SimThreadInfo::setup_worker_ids(id,tid);
#ifndef TWO_PROCESS_RPC
    index_thread(tid);
#else
    // RPC has one client process and one server process.
    if(!id) { ++local_ready; return; }
#endif
    utils::Properties props; std::ifstream spec(argv[8]); props.Load(spec);
    props.SetProperty("recordcount",std::to_string(records));
    props.SetProperty("operationcount",std::to_string(ops));
    props.SetProperty("insertstart","0");
    ycsbc::CoreWorkload workload; workload.Init(props);
    uint64_t count=ops/threads+(uint64_t(t)<ops%threads);
    std::vector<Op> trace; trace.reserve(count);
    for(uint64_t i=0;i<count;++i) {
      bool write=workload.NextOperation()==ycsbc::UPDATE;
      uint64_t key=workload.NextTransactionNumKey()+1;
      // Original BwTree adapter maps UPDATE to a random fresh-key insertion.
      // Use disjoint deterministic keys when fresh insertion is selected.
      if(write && fresh) key=records+1+((i*threads+t)*2+id);
      trace.push_back({key,write});
    }
    ++local_ready; wait_for(shared->start,token);
    uint64_t request_sequence=0;
    for(const auto& op:trace) {
#ifdef TWO_PROCESS_RPC
      auto& m=shared->mail[t]; m.key=op.key; m.write=op.write; m.sequence=++request_sequence;
      publish(&m.key,4*sizeof(uint64_t)); shared->requests->enqueue(uint32_t(t));
      while(m.response_sequence.load()!=request_sequence) shm_platform::pause();
      clflush(&m.ok,sizeof(m.ok)); shm_platform::full_fence(); if(!m.ok) ++errors;
#else
      if(op.write) write_key(op.key);
      else if(!get_checked(op.key)) ++errors;
#endif
      if(op.write) shared->witness[id][t]=op.key;
      ++completed;
    }
#ifndef TWO_PROCESS_RPC
    index_finish(tid);
#endif
  });
  while(local_ready.load()!=threads) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  shared->ready[id].store(token); wait_for(shared->ready[1-id],token);
  auto begin=std::chrono::steady_clock::now(); if(!id) shared->start.store(token);
  for(auto& t:workers) t.join();
#ifdef TWO_PROCESS_RPC
  if(!id) {wait_for(shared->done[1],token); stop_servers=true; for(auto& t:servers)t.join();}
#endif
  shared->seconds[id]=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
  // Untimed cross-process witness: each process publishes a unique new key.
  SimThreadInfo::setup_worker_ids(id,thread_offset); index_thread(thread_offset);
  index_put(records+2*ops+10+id,allocate_value(records+2*ops+10+id));
  index_finish(thread_offset);
  shared->completed[id]=completed; shared->errors[id]=errors;
  publish(&shared->completed[0],sizeof(shared->completed)+sizeof(shared->errors));
  publish(shared->seconds,sizeof(shared->seconds));
  publish(shared->witness[id],sizeof(shared->witness[id]));
  shared->done[id].store(token); wait_for(shared->done[1-id],token);
  SimThreadInfo::setup_worker_ids(id,thread_offset); index_thread(thread_offset);
  // Both processes independently verify the entire common initial population.
  for(uint64_t k=1;k<=records;++k) if(!get_checked(k)) ++errors;
  const bool peer_marker_ok=get_checked(records+2*ops+10+(1-id));
  if(!peer_marker_ok) ++errors;
  unsigned peer_writes=0;
  for(int t=0;t<counts[1-id];++t) {
    clflush(&shared->witness[1-id][t],sizeof(shared->witness[1-id][t])); shm_platform::full_fence();
    const auto key=shared->witness[1-id][t];
    if(key) { ++peer_writes; if(!get_checked(key)) ++errors; }
  }
  index_finish(thread_offset);
  std::cout<<"RESULT system="<<BENCH_SYSTEM<<" cc="<<BENCH_CC<<" opt="<<BENCH_OPT<<" id="<<id<<" pid="<<getpid()<<" threads="<<threads
    <<" completed="<<completed<<" errors="<<errors<<" seconds="<<shared->seconds[id]
    <<" peer_marker_checked="<<peer_marker_ok<<" peer_writes_checked="<<peer_writes<<" validation_records="<<records<<" update_semantics="<<(fresh?"fresh-insert":"update-existing")<<" reclamation=process-lifetime"<<std::endl;
#ifdef SHM_UB_SPLIT
  index_close();
#endif
  shared->validated[id].store(token); wait_for(shared->validated[1-id],token);
  // Shared data remains alive until both workers finish; do not run a shared
  // object's std::thread/memkind destructor in another process.
#ifndef SHM_UB_SPLIT
  index_close();
#endif
#ifdef SHM_UB_SPLIT
  std::cout << "UB_ALLOCATIONS data_bytes=" << ub_arena_used(false)
            << " atomic_bytes=" << ub_arena_used(true) << std::endl;
#endif
  mapper->close_ub();
#ifdef SHM_UB_SPLIT
  nc_mapper->close_ub();
#endif
  std::cout << "UB_CLOSED api=" << ub_api_name() << " id=" << id << std::endl;
  std::cout.flush(); std::cerr.flush(); _Exit(errors?1:0);
} catch(const std::exception& e) { std::cerr<<"ERROR "<<e.what()<<std::endl; std::cerr.flush(); _Exit(1); }
