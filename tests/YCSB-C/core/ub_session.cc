#include "core/ub_session.h"
#ifdef YCSB_UB_TWO_HOST
#include "core/ub_cpu.h"
#include "core/ub_numa.h"
#include "db/ub_db.h"
#include "shm/mm.h"
#include "shm/ub.h"
#include "utils/bypass_cache.h"
#include "utils/sim_id.h"
#if defined(ENABLE_BWTREE_DB) && defined(NO_CC) && BWTREE_PROCESS_MAPPING_CACHE
#include "process_mapping_cache.h"
#endif
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <climits>
#include <mutex>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <numa.h>
#include <numaif.h>
#include <cstring>
namespace ycsbc_ub {
namespace {
constexpr uintptr_t data_base=0x300000000000, nc_base=0x400000000000;
void *data=nullptr,*nc=nullptr;
size_t data_size=0,nc_size=0;
std::string name;
bool owner=false;
bool all_nc=false, direct_nc=false;
bool rpc=false;
void* local_index_base=nullptr;
class LocalIndexMapper final : public SystemMemoryMmapper {
public:
  void allocate(void*, size_t bytes) override {
    mmap_size=bytes;
    mmap_base=allocate_local_dram(bytes);
    local_index_base=mmap_base;
  }
};
std::vector<int> worker_cpus, rpc_worker_cpus;
const char* env(const char* key,const char* fallback=nullptr) {
  const char* value=std::getenv(key);
  if((!value || !*value) && fallback) value=std::getenv(fallback);
  if(!value || !*value) throw std::runtime_error(std::string("Missing ")+key);
  return value;
}
#ifdef YCSB_CXL_TWO_PROCESS
std::string cxl_directory;
int cxl_node=0;
CXLSystemMemoryMmapper* readonly_data_mapper=nullptr;
// Reuse the original CXL mapper and memkind allocator. Keep the A/B control
// header outside both allocation slices, as in the UB layout.
class CxlSessionMapper final : public CXLSystemMemoryMmapper {
  bool metadata_;
public:
  CxlSessionMapper(const char* path,bool metadata)
      : CXLSystemMemoryMmapper(path,metadata?2:1,metadata&&!owner?1:0),metadata_(metadata) {}
  void allocate(void* base,size_t size) override {
    CXLSystemMemoryMmapper::allocate(base,size);
    auto* mask=numa_allocate_nodemask();
    numa_bitmask_setbit(mask,cxl_node);
    const int rc=mbind(mmap_base,mmap_size,MPOL_BIND,mask->maskp,mask->size+1,MPOL_MF_STRICT);
    const int saved_errno=errno;
    numa_free_nodemask(mask);
    if(rc) throw std::runtime_error(std::string("Cannot bind CXL mapping: ")+std::strerror(saved_errno));
    if(metadata_) {
      per_machine_size=((size-4096)/2)&~size_t(4095);
      per_machine_base=static_cast<char*>(mmap_base)+4096+(owner?0:per_machine_size);
    }
  }
};
std::string cxl_path(bool metadata) { return cxl_directory+"/"+name+(metadata?"-nc":""); }
void create_cxl_region(bool metadata,size_t bytes) {
  const auto path=cxl_path(metadata);
  int fd=open(path.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);
  if(fd<0) throw std::runtime_error("Cannot create CXL backing file: "+path);
  const int rc=ftruncate(fd,bytes); close(fd);
  if(rc) throw std::runtime_error("Cannot size CXL backing file: "+path);
}
void prepare_cxl() {
  cxl_directory=env("YCSB_CXL_DIRECTORY");
  cxl_node=std::stoi(env("YCSB_CXL_NODE"));
  if(numa_available()<0 || cxl_node<0 || cxl_node>numa_max_node() ||
      !numa_bitmask_isbitset(numa_all_nodes_ptr,cxl_node))
    throw std::runtime_error("Requested CXL NUMA node is unavailable");
  if(owner) { if(!rpc && !all_nc) create_cxl_region(false,data_size); create_cxl_region(true,nc_size); }
  ub_atomic_memory=MemoryManager(new CxlSessionMapper(cxl_path(true).c_str(),true),
                                reinterpret_cast<void*>(nc_base),nc_size);
  // B owns the shared index and reclaims its allocations after the existing
  // cache-invalidation/epoch grace period. Keep strict shared allocation, but
  // allow memkind to reuse memory instead of retaining every obsolete node.
  ub_atomic_memory.require_shared_allocations();
  nc=reinterpret_cast<void*>(nc_base);
  if(!rpc && !all_nc) {
    if(owner) {
      cacheable=MemoryManager(new CxlSessionMapper(cxl_path(false).c_str(),false),
                             reinterpret_cast<void*>(data_base),data_size);
      cacheable.require_shared_allocations();
    } else {
      readonly_data_mapper=new CxlSessionMapper(cxl_path(false).c_str(),false);
      readonly_data_mapper->allocate(reinterpret_cast<void*>(data_base),data_size);
      if(mprotect(readonly_data_mapper->mmap_base,data_size,PROT_READ))
        throw std::runtime_error("Cannot make A's CXL node mapping read-only");
      // Preserve the existing guard against A allocating node/value storage.
      cacheable=MemoryManager(false);
    }
    data=reinterpret_cast<void*>(data_base);
  }
  std::cerr<<"CXL_MAPPING role="<<(owner?"B":"A")<<" node="<<cxl_node
           <<" allocator=memkind atomic_backend=queue cache_maintenance=native CACHE="<<((rpc||all_nc)?"NONE":owner?"RW":"READ")
           <<" metadata=RW"<<std::endl;
}
void check_cxl_placement() {
  for(void* address:{nc,data}) {
    if(!address) continue;
    int actual=-1;
    if(get_mempolicy(&actual,nullptr,0,address,MPOL_F_ADDR|MPOL_F_NODE) || actual!=cxl_node)
      throw std::runtime_error("CXL mapping did not allocate on the requested NUMA node");
    std::cerr<<"CXL_PLACEMENT address="<<address<<" first_page_node="<<actual<<std::endl;
  }
}
#endif
int control_fd() {
  static const int fd=[] {
    std::string value=env("YCSB_UB_CONTROL_FD");
    size_t end=0;
    int descriptor=std::stoi(value,&end);
    if(end!=value.size() || descriptor<3 || fcntl(descriptor,F_GETFD)==-1)
      throw std::runtime_error("Invalid YCSB_UB_CONTROL_FD");
    return descriptor;
  }();
  return fd;
}
volatile sig_atomic_t fault_fd=-1;
// Signal context: stack buffer, integer formatting, write and kill only.
void fault_handler(int signal_number, siginfo_t* info, void* context) {
  char buffer[256]; size_t used=0;
  auto literal=[&](const char* s) { while(*s) buffer[used++]=*s++; };
  auto hex=[&](uintptr_t value) {
    char digits[2*sizeof(value)]; size_t n=0;
    do { digits[n++]="0123456789abcdef"[value&15]; value>>=4; } while(value);
    literal("0x"); while(n) buffer[used++]=digits[--n];
  };
  uintptr_t pc=0;
#if defined(__aarch64__)
  pc=static_cast<ucontext_t*>(context)->uc_mcontext.pc;
#elif defined(__x86_64__)
  pc=static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP];
#endif
  literal("FAULT signal="); hex(signal_number);
  literal(" code="); hex(info->si_code);
  literal(" address="); hex(reinterpret_cast<uintptr_t>(info->si_addr));
  literal(" pc="); hex(pc); literal("\n");
  const auto ignored=write(fault_fd,buffer,used); (void)ignored;
  // SA_RESETHAND restored the default disposition. Preserve signal exit/core semantics.
  kill(getpid(),signal_number);
}
void install_fault_reporter() {
  fault_fd=control_fd();
  struct sigaction action{};
  action.sa_sigaction=fault_handler;
  action.sa_flags=SA_SIGINFO|SA_RESETHAND;
  sigemptyset(&action.sa_mask);
  if(sigaction(SIGSEGV,&action,nullptr) || sigaction(SIGBUS,&action,nullptr))
    throw std::runtime_error("Cannot install UB worker fault reporter");
}
void command(const char* expected) {
  std::string value;
  if(!std::getline(std::cin,value) || value!=expected)
    throw std::runtime_error(std::string("Expected controller command: ")+expected);
}
size_t megabytes(const char* key) {
  auto mb=std::stoull(env(key));
  if(mb<128 || mb%4 || mb>(nc_base-data_base)/(1UL<<20))
    throw std::runtime_error(std::string("Invalid ")+key);
  return mb<<20;
}
}
void* allocate_local_dram(size_t bytes) {
  if (!bytes) return nullptr;
  const int node = local_memory_node();
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) throw std::bad_alloc();
  auto* mask = numa_allocate_nodemask();
  if (!mask) { munmap(p, bytes); throw std::bad_alloc(); }
  numa_bitmask_setbit(mask, node);
  const int rc = mbind(p, bytes, MPOL_BIND, mask->maskp, mask->size, MPOL_MF_STRICT);
  const int saved_errno = errno;
  numa_free_nodemask(mask);
  if (rc) {
    munmap(p, bytes);
    throw std::runtime_error(std::string("Cannot bind workload to local DRAM: ") + std::strerror(saved_errno));
  }
  return p;
}
void free_local_dram(void* address, size_t bytes) noexcept {
  if (address && bytes) munmap(address, bytes);
}
void check_local_workload(const void* address, size_t bytes, unsigned thread, size_t operations) {
  const int cpu_node = numa_node_of_cpu(sched_getcpu());
  const int node = local_memory_node(cpu_node);
  if (bytes) {
    const auto first = reinterpret_cast<uintptr_t>(address);
    if ((first >= data_base && first - data_base < data_size) ||
        (first >= nc_base && first - nc_base < nc_size))
      throw std::runtime_error("Workload descriptors must not reside in shared memory");
    for (const void* page : {address, static_cast<const void*>(static_cast<const char*>(address) + bytes - 1)}) {
      int actual = -1;
      if (get_mempolicy(&actual, nullptr, 0, const_cast<void*>(page), MPOL_F_ADDR | MPOL_F_NODE) || actual != node)
        throw std::runtime_error("Workload descriptors are not on the selected local memory node");
    }
  }
  std::ostringstream message;
  message << "WORKLOAD_STORAGE thread=" << thread << " operations=" << operations
          << " storage=local-dram node=" << node << " cpu_node=" << cpu_node
          << " address=" << address << " bytes=" << bytes;
  stage(message.str());
}
void stage(const std::string& message) {
  static std::mutex log_mutex;
  std::lock_guard<std::mutex> guard(log_mutex);
  std::cerr << "YCSB_STAGE " << message << std::endl;
}
void reply(const std::string& message) {
  if(message.find('\n')!=std::string::npos || message.size()>4095)
    throw std::runtime_error("Invalid controller reply");
  const std::string line=message+"\n";
  static std::mutex reply_mutex;
  std::lock_guard<std::mutex> guard(reply_mutex);
  size_t sent=0;
  while(sent<line.size()) {
    ssize_t n=write(control_fd(),line.data()+sent,line.size()-sent);
    if(n<0 && errno==EINTR) continue;
    if(n<=0) throw std::runtime_error("Controller reply pipe write failed");
    sent+=static_cast<size_t>(n);
  }
}
int threads(int machine) {
  int n=std::stoi(env(machine==0?"YCSB_UB_THREADS_B":"YCSB_UB_THREADS_A"));
  if(n<1 || n>192) throw std::runtime_error("Threads must be 1..192 per host");
  return n;
}
int first_thread(int machine) { return machine==0?0:threads(0); }
int worker_cpu(unsigned local_index) { return worker_cpus.at(local_index); }
int cpu_start() { return worker_cpu(0); }
int rpc_worker_cpu(unsigned i) { return rpc_worker_cpus.at(i); }
bool nc_data() { return all_nc; }
bool nc_direct_data() { return direct_nc; }
bool rpc_mode() { return rpc; }
bool in_local_index(const void* address, size_t bytes) {
  auto p=reinterpret_cast<uintptr_t>(address), base=reinterpret_cast<uintptr_t>(local_index_base);
  return base && p>=base && p-base<data_size && bytes<=data_size-(p-base);
}
void prepare(utils::Properties& props) {
  (void)control_fd(); // Fail before allocating UB objects if launched without a controller.
  install_fault_reporter();
#ifdef YCSB_CXL_TWO_PROCESS
  if(std::string(env("YCSB_BACKEND"))!="cxl") throw std::runtime_error("CXL binary requires backend=cxl");
  const char* backend="CXL queue";
#else
  if(std::getenv("YCSB_BACKEND") && std::string(std::getenv("YCSB_BACKEND"))!="ub")
    throw std::runtime_error("UB binary requires backend=ub; rebuild the CXL bundle");
  const char* backend="UB";
#endif
  stage(std::string("prepare ")+backend+" session");
  const char* writable=std::getenv("YCSB_UB_A_WRITABLE");
  if(writable && std::string(writable)!="0")
    throw std::runtime_error("A is always read-only; YCSB_UB_A_WRITABLE must be 0");
  owner=SimThreadInfo::is_master();
  rpc=std::getenv("YCSB_UB_RPC") && std::string(std::getenv("YCSB_UB_RPC"))=="1";
#if defined(USE_MSG_QUEUE)
  if(!rpc) throw std::runtime_error("build.sh rpc requires YCSB_UB_RPC=1");
#else
  if(rpc) throw std::runtime_error("RPC requires build.sh rpc; rebuild the bundle");
#endif
  worker_cpus=parse_worker_cpus(std::getenv("YCSB_UB_CPUS"),threads(owner?0:1),
                                std::stoi(env("YCSB_UB_CPU")));
  if(rpc) rpc_worker_cpus=parse_worker_cpus(env("YCSB_RPC_CPUS"),threads(owner?0:1),0);
  cpu_set_t allowed;
  if(sched_getaffinity(0,sizeof(allowed),&allowed))
    throw std::runtime_error("Cannot read UB worker CPU affinity");
  for(int cpu:worker_cpus)
    if(!CPU_ISSET(cpu,&allowed)) throw std::runtime_error("Requested UB worker CPU is unavailable");
  const char* access=std::getenv("YCSB_UB_SHERMAN_ACCESS");
  all_nc=props["dbname"]=="sherman" && access && std::string(access)!="cache";
  direct_nc=all_nc && std::string(access)=="nc-direct";
  name=env("YCSB_UB_NAME");
  data_size=megabytes("YCSB_UB_SIZE_MB"); nc_size=megabytes("YCSB_UB_NC_SIZE_MB");
  if(SimThreadInfo::worker_machine_count!=2 ||
     SimThreadInfo::worker_db_count!=1 || SimThreadInfo::worker_thread_count!=unsigned(threads(0)+threads(1)))
    throw std::runtime_error("UB mode requires one shared index, two hosts, and matching total threads");
  const auto records=std::stoull(env("YCSB_UB_RECORDS"));
  const auto ops=std::stoull(env("YCSB_UB_OPS"));
  if(records<2 || records>INT_MAX || ops<1 || ops>INT_MAX/2)
    throw std::runtime_error("Invalid record/operation count");
  props.SetProperty("recordcount",std::to_string(records));
  props.SetProperty("operationcount",std::to_string(2*ops));
  double reads=std::stod(props.GetProperty("readproportion","0"));
  double writes=std::stod(props.GetProperty("updateproportion","0"));
  if(reads!=0.9 || writes!=0.1) throw std::runtime_error("Expected workload: 90% read / 10% update");
  for(const char* key:{"insertproportion","scanproportion","readmodifywriteproportion"})
    if(std::stod(props.GetProperty(key,"0"))!=0) throw std::runtime_error("Only read/update are supported");
#ifdef YCSB_CXL_TWO_PROCESS
  prepare_cxl();
#else
  if(owner) {
    if(!all_nc && !rpc) create_ub_memory(name.c_str(),data_size,env("YCSB_UB_CACHE_PROVIDER","YCSB_UB_PROVIDER"),UINT32_MAX,UbMappingMode::Cacheable);
    create_ub_memory((name+"-nc").c_str(),nc_size,env("YCSB_UB_NC_PROVIDER","YCSB_UB_PROVIDER"),UINT32_MAX,UbMappingMode::Noncacheable);
  }
  nc=map_ub_memory((name+"-nc").c_str(),reinterpret_cast<void*>(nc_base),nc_size,UbMappingMode::Noncacheable);
  if(!all_nc && !rpc) data=map_ub_memory(name.c_str(),reinterpret_cast<void*>(data_base),data_size,UbMappingMode::Cacheable,!owner);
  size_t half=((nc_size-4096)/2)&~size_t(all_nc ? 4095 : 63);
  ub_configure_arena(true,static_cast<char*>(nc)+4096+(owner?0:half),half);
  if(!rpc) {
    if(!all_nc && owner) ub_configure_arena(false,data,data_size);
    cacheable=MemoryManager(all_nc);
  }
#endif
  // RPC partitions use local DRAM with either shared transport backend.
  // A never allocates shared CACHE data in direct mode.
  // BwTree returns retired CACHE nodes through its epoch GC (and, for OPT,
  // mapping-cache invalidation ACKs plus a second reader grace period).
  // The arena must recycle those safely retired allocations.
  if(rpc) {
    cacheable=MemoryManager(new LocalIndexMapper,nullptr,data_size);
    // This index is private to this process. Honour frees after its epoch GC;
    // retaining them until exit exhausts the fixed pool during load/updates.
    cacheable.require_shared_allocations();
    std::cerr<<"RPC_INDEX_STORAGE role="<<(owner?"B":"A")<<" storage=local-dram base="<<local_index_base
             <<" bytes="<<data_size<<" node="<<local_memory_node()
             <<" cpu_node="<<numa_node_of_cpu(sched_getcpu())<<std::endl;
  }
  stage(std::string(backend)+" mappings ready; allocators configured");
  std::cerr<<"YCSB framework "<<backend<<" role="<<(owner?"B":"A")<<" CACHE="<<((all_nc||rpc)?"NONE":owner?"RW":"READ")<<" metadata=RW"<<std::endl;
}
void* control_header() { return nc; }
void ready() {
#ifdef YCSB_CXL_TWO_PROCESS
  check_cxl_placement();
#endif
  mfence();
  reply("READY");
  command("run");
}
void await_cleanup() { command(owner?"release":"close"); }
void finish() {
  ycsbc::ReleaseLocalUbDBs();
#if defined(ENABLE_BWTREE_DB) && defined(NO_CC) && BWTREE_PROCESS_MAPPING_CACHE
  // A does not destroy B's shared tree. Stop its local invalidation receiver
  // and retire any private cache before either shared mapping is unmapped (OPT/NOOPT).
  bwtree_process_cache::shutdown();
#endif
#ifdef YCSB_CXL_TWO_PROCESS
  cacheable=MemoryManager();
  local_index_base=nullptr;
  delete readonly_data_mapper; readonly_data_mapper=nullptr;
  ub_atomic_memory=MemoryManager(true);
  data=nullptr; nc=nullptr;
  if(owner) {
    if((!rpc && !all_nc && unlink(cxl_path(false).c_str())) || unlink(cxl_path(true).c_str()))
      throw std::runtime_error("Cannot remove this round's CXL backing files");
  }
#else
  if(rpc) { cacheable=MemoryManager(); local_index_base=nullptr; }
  if(data) { unmap_ub_memory(data,data_size); data=nullptr; }
  unmap_ub_memory(nc,nc_size); nc=nullptr;
  if(owner) {
    if(!all_nc && !rpc) destroy_ub_memory(name.c_str());
    destroy_ub_memory((name+"-nc").c_str());
  }
#endif
  reply("CLOSED");
}
}
#endif
