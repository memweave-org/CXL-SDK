// Untimed, bounded correctness assertions against the file-backed SDK fixture.
// Both processes insert, update and read the same NC-mapped tree. No throughput.
#include "Tree.h"
#include "shm/ub.h"
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
constexpr size_t bytes=64ul<<20;
struct Handles { nt<uint64_t>* locks; nt<void*>* roots; void* sequences; };
void transfer(int fd,void* p,size_t n,bool writing) {
  auto* b=static_cast<char*>(p);
  while(n) {
    ssize_t count=writing ? ::write(fd,b,n) : ::read(fd,b,n);
    if(count<0 && errno==EINTR) continue;
    if(count<=0) throw std::runtime_error("contract peer disconnected");
    n-=count;b+=count;
  }
}
void barrier(int in,int out) { char token=1;transfer(out,&token,1,true);transfer(in,&token,1,false); }
void run(bool owner,bool direct,int in,int out,const char* name) {
  if(owner) create_ub_memory(name,bytes,"fixture",UINT32_MAX,UbMappingMode::Noncacheable);
  char token=1;
  transfer(owner?out:in,&token,1,owner);
  auto* region=static_cast<char*>(map_ub_memory(name,reinterpret_cast<void*>(0x400000000000ul),bytes,UbMappingMode::Noncacheable));
  const size_t half=((bytes-4096)/2)&~size_t(4095);
  ub_configure_arena(true,region+4096+(owner?0:half),half);
  cxl_transport::configure_nc_nodes(true,direct);
  Handles handles{};
  if(owner) {
    handles.sequences=cxl_transport::allocate_atomic(cxl_transport::shared_sequences_bytes());
    std::memset(handles.sequences,0,cxl_transport::shared_sequences_bytes());
  } else {
    transfer(in,&handles,sizeof(handles),false);
    global_lock_table=handles.locks;root_ptr_list=handles.roots;
  }
  cxl_transport::configure_shared_sequences(handles.sequences);
  DSMConfig config;config.machineNR=1;config.cacheConfig.cacheSize=1;
  DSM* dsm=DSM::getInstance(config);
  dsm->set_process_id(owner?0:1);dsm->set_direct_nc_reads(direct);dsm->registerThread();
  Tree tree(dsm,0,owner);tree.register_thread(0);
  if(owner) {
    tree.insert(1,1001);tree.insert(2,1002);
    handles.locks=global_lock_table;handles.roots=root_ptr_list;
    transfer(out,&handles,sizeof(handles),true);
  }
  barrier(in,out);
  for(uint64_t k=owner?3:4;k<=1024;k+=2) tree.insert(k,k+1000);
  barrier(in,out);
  for(uint64_t k=1;k<=1024;++k) {
    Value value=0;
    if(!tree.search(k,value) || value!=k+1000) throw std::runtime_error("split lost key/value "+std::to_string(k));
  }
  barrier(in,out);
  for(uint64_t k=owner?1:2;k<=1024;k+=2) tree.insert(k,k+2000);
  barrier(in,out);
  for(uint64_t k=1;k<=1024;++k) {
    Value value=0;
    if(!tree.search(k,value) || value!=k+2000) throw std::runtime_error("update lost key/value "+std::to_string(k));
  }
  barrier(in,out);
  unmap_ub_memory(region,bytes);
  barrier(in,out);
  if(owner) destroy_ub_memory(name);
}
}

int main(int argc,char** argv) {
  if(argc!=2 || !std::getenv("UBSM_TEST_DIRECTORY")) {
    std::cerr<<"Requires fixture SDK, UBSM_TEST_DIRECTORY and direct|memcpy\n";return 2;
  }
  const std::string mode=argv[1];
  if(mode!="direct" && mode!="memcpy") return 2;
  int ab[2],ba[2];if(pipe(ab) || pipe(ba)) return 2;
  const std::string name="sherman-nc-contract-"+std::to_string(getpid());
  const pid_t child=fork();if(child<0) return 2;
  alarm(30); // Failure watchdog only; no elapsed-time result or performance threshold.
  try {
    if(child==0) {
      close(ab[1]);close(ba[0]);run(false,mode=="direct",ab[0],ba[1],name.c_str());_exit(0);
    }
    close(ab[0]);close(ba[1]);run(true,mode=="direct",ba[0],ab[1],name.c_str());
    int status=0;waitpid(child,&status,0);
    if(!WIFEXITED(status) || WEXITSTATUS(status)) throw std::runtime_error("child failed");
    std::cout<<"PASS NC "<<mode<<": symmetric two-process inserts, splits, updates and reads\n";
  } catch(const std::exception& e) {
    std::cerr<<e.what()<<'\n';
    if(child>0) { kill(child,SIGKILL);waitpid(child,nullptr,0); }
    return 1;
  }
}
