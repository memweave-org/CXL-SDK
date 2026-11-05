#include "core/ub_session.h"
#ifdef YCSB_UB_TWO_HOST
#include "core/ub_cpu.h"
#include "shm/mm.h"
#include "shm/ub.h"
#include "utils/bypass_cache.h"
#include "utils/sim_id.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <climits>
#include <mutex>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
namespace ycsbc_ub {
namespace {
constexpr uintptr_t data_base=0x300000000000, nc_base=0x400000000000;
void *data=nullptr,*nc=nullptr;
size_t data_size=0,nc_size=0;
std::string name;
bool owner=false;
bool all_nc=false, direct_nc=false;
std::vector<int> worker_cpus;
const char* env(const char* key) {
  const char* value=std::getenv(key);
  if(!value || !*value) throw std::runtime_error(std::string("Missing ")+key);
  return value;
}
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
bool nc_data() { return all_nc; }
bool nc_direct_data() { return direct_nc; }
void prepare(utils::Properties& props) {
  (void)control_fd(); // Fail before allocating UB objects if launched without a controller.
  install_fault_reporter();
  stage("prepare UB session");
  const char* writable=std::getenv("YCSB_UB_A_WRITABLE");
  if(writable && std::string(writable)!="0")
    throw std::runtime_error("A is always read-only; YCSB_UB_A_WRITABLE must be 0");
  owner=SimThreadInfo::is_master();
  worker_cpus=parse_worker_cpus(std::getenv("YCSB_UB_CPUS"),threads(owner?0:1),
                                std::stoi(env("YCSB_UB_CPU")));
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
  if(owner) {
    if(!all_nc) create_ub_memory(name.c_str(),data_size,env("YCSB_UB_PROVIDER"),UINT32_MAX,UbMappingMode::Cacheable);
    create_ub_memory((name+"-nc").c_str(),nc_size,env("YCSB_UB_PROVIDER"),UINT32_MAX,UbMappingMode::Noncacheable);
  }
  nc=map_ub_memory((name+"-nc").c_str(),reinterpret_cast<void*>(nc_base),nc_size,UbMappingMode::Noncacheable);
  if(!all_nc) data=map_ub_memory(name.c_str(),reinterpret_cast<void*>(data_base),data_size,UbMappingMode::Cacheable,!owner);
  size_t half=((nc_size-4096)/2)&~size_t(all_nc ? 4095 : 63);
  ub_configure_arena(true,static_cast<char*>(nc)+4096+(owner?0:half),half);
  // A never allocates data. Leaving its CACHE allocator unconfigured catches violations.
  if(!all_nc && owner) ub_configure_arena(false,data,data_size);
  cacheable=MemoryManager(all_nc);
  stage("UB mappings ready; allocators configured");
  std::cerr<<"YCSB framework UB role="<<(owner?"B":"A")<<" CACHE="<<(all_nc?"NONE":owner?"RW":"READ")<<" NC=RW"<<std::endl;
}
void* control_header() { return nc; }
void ready() {
  mfence();
  reply("READY");
  command("run");
}
void await_cleanup() { command(owner?"release":"close"); }
void finish() {
  if(data) { unmap_ub_memory(data,data_size); data=nullptr; }
  unmap_ub_memory(nc,nc_size); nc=nullptr;
  if(owner) {
    if(!all_nc) destroy_ub_memory(name.c_str());
    destroy_ub_memory((name+"-nc").c_str());
  }
  reply("CLOSED");
}
}
#endif
