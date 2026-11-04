#include "core/ub_session.h"
#ifdef YCSB_UB_TWO_HOST
#include "shm/mm.h"
#include "shm/ub.h"
#include "utils/bypass_cache.h"
#include "utils/sim_id.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <climits>
namespace ycsbc_ub {
namespace {
constexpr uintptr_t data_base=0x300000000000, nc_base=0x400000000000;
void *data=nullptr,*nc=nullptr;
size_t data_size=0,nc_size=0;
std::string name;
bool owner=false;
const char* env(const char* key) {
  const char* value=std::getenv(key);
  if(!value || !*value) throw std::runtime_error(std::string("Missing ")+key);
  return value;
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
int threads(int machine) {
  int n=std::stoi(env(machine==0?"YCSB_UB_THREADS_B":"YCSB_UB_THREADS_A"));
  if(n<1 || n>192) throw std::runtime_error("Threads must be 1..192 per host");
  return n;
}
int first_thread(int machine) { return machine==0?0:threads(0); }
int cpu_start() { return std::stoi(env("YCSB_UB_CPU")); }
void prepare(utils::Properties& props) {
  owner=SimThreadInfo::is_master();
  name=env("YCSB_UB_NAME");
  data_size=megabytes("YCSB_UB_SIZE_MB"); nc_size=megabytes("YCSB_UB_NC_SIZE_MB");
  if(props.GetProperty("dbname")!="bwtree" || SimThreadInfo::worker_machine_count!=2 ||
     SimThreadInfo::worker_db_count!=1 || SimThreadInfo::worker_thread_count!=unsigned(threads(0)+threads(1)))
    throw std::runtime_error("UB mode requires one shared BwTree, two hosts, and matching total threads");
  const auto records=std::stoull(env("YCSB_UB_RECORDS"));
  const auto ops=std::stoull(env("YCSB_UB_OPS"));
  if(records<2 || records>INT_MAX || ops<1 || ops>INT_MAX/2)
    throw std::runtime_error("Invalid record/operation count");
  props.SetProperty("recordcount",std::to_string(records));
  props.SetProperty("operationcount",std::to_string(2*ops));
  double reads=std::stod(props.GetProperty("readproportion","0"));
  double writes=std::stod(props.GetProperty("updateproportion","0"));
  if(reads!=0.9 || writes!=0.1) throw std::runtime_error("Expected B workload: 90% read / 10% update");
  for(const char* key:{"insertproportion","scanproportion","readmodifywriteproportion"})
    if(std::stod(props.GetProperty(key,"0"))!=0) throw std::runtime_error("Only read/update are supported");
  if(owner) {
    create_ub_memory(name.c_str(),data_size,env("YCSB_UB_PROVIDER"),UINT32_MAX,UbMappingMode::Cacheable);
    create_ub_memory((name+"-nc").c_str(),nc_size,env("YCSB_UB_PROVIDER"),UINT32_MAX,UbMappingMode::Noncacheable);
  }
  nc=map_ub_memory((name+"-nc").c_str(),reinterpret_cast<void*>(nc_base),nc_size,UbMappingMode::Noncacheable);
  data=map_ub_memory(name.c_str(),reinterpret_cast<void*>(data_base),data_size,UbMappingMode::Cacheable,!owner);
  size_t half=((nc_size-4096)/2)&~size_t(63);
  ub_configure_arena(true,static_cast<char*>(nc)+4096+(owner?0:half),half);
  // A never allocates data. Leaving its CACHE allocator unconfigured catches violations.
  if(owner) ub_configure_arena(false,data,data_size);
  cacheable=MemoryManager(false);
  std::cerr<<"YCSB framework UB role="<<(owner?"B":"A")<<" CACHE="<<(owner?"RW":"READ")<<" NC=RW"<<std::endl;
}
void* control_header() { return nc; }
void ready() {
  mfence();
  std::cout<<"UBYCSB READY"<<std::endl;
  command("run");
}
void await_cleanup() { command(owner?"release":"close"); }
void finish() {
  unmap_ub_memory(data,data_size); data=nullptr;
  unmap_ub_memory(nc,nc_size); nc=nullptr;
  if(owner) { destroy_ub_memory(name.c_str());destroy_ub_memory((name+"-nc").c_str()); }
  std::cout<<"UBYCSB CLOSED"<<std::endl;
}
}
#endif
