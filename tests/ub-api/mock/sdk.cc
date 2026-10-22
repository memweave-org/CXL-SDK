#include "ubs_mem.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <filesystem>
#include <fstream>
static bool active;
struct Mapping { size_t bytes; std::string path; uint64_t flags; };
static std::map<void*, Mapping> maps;
static std::string root() { const char* p=getenv("UBSM_TEST_DIRECTORY"); return p?p:"/tmp/cxl-ubsm-stub"; }
static std::string path(const char* n) { return root()+"/"+n; }
static int event(const char* op) {
  fprintf(stderr,"UBSM_TEST_STUB %s pid=%ld\n",op,long(getpid()));
  const char* hang=getenv("UBSM_TEST_HANG");
  if(hang && !strcmp(hang,op)) for(;;) pause();
  const char* fail=getenv("UBSM_TEST_FAIL");
  return fail && !strcmp(fail,op) ? 6001 : 0;
}
extern "C" int ubsmem_init_attributes(ubsmem_options_t*) { return event("attributes"); }
extern "C" int ubsmem_initialize(const ubsmem_options_t*) { int rc=event("initialize"); if(!rc)active=true;return rc; }
extern "C" int ubsmem_finalize() {
  if(!active || !maps.empty())return 6002;
  int rc=event("finalize");if(!rc)active=false;return rc;
}
extern "C" int ubsmem_shmem_allocate_with_provider(const ubs_mem_provider_t* p,const char* n,size_t sz,mode_t mode,uint64_t flags) {
  if(!active || !p->host_name[0] || p->socket_id!=UINT32_MAX || p->port_id!=UINT32_MAX || (flags!=3 && flags!=4 && flags!=10) || mode!=0660)return 6002;
  if(int rc=event("allocate"))return rc;
  std::filesystem::create_directories(root());
  int fd=open(path(n).c_str(),O_CREAT|O_EXCL|O_RDWR,mode);if(fd<0)return 6003;
  int rc=ftruncate(fd,sz);close(fd);
  if(!rc) std::ofstream(path(n)+".flags") << flags;
  return rc?6004:0;
}
extern "C" int ubsmem_shmem_map(void* addr,size_t sz,int prot,int flags,const char* n,off_t offset,void** out) {
  if(!active || offset || prot!=(PROT_READ|PROT_WRITE) || (addr && !(flags&MAP_FIXED_NOREPLACE)))return 6002;
  if(int rc=event("map"))return rc;
  int fd=open(path(n).c_str(),O_RDWR);if(fd<0)return 6003;
  struct stat st{};fstat(fd,&st);if(size_t(st.st_size)!=sz){close(fd);return 6004;}
  if(getenv("UBSM_TEST_WRONG_ADDRESS")) {addr=nullptr;flags=MAP_SHARED;}
  void* p=mmap(addr,sz,prot,flags,fd,offset);close(fd);if(p==MAP_FAILED)return 6005;
  std::string marker=path(n)+".mapped."+std::to_string(getpid());
  fd=open(marker.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);if(fd<0){munmap(p,sz);return 6006;}close(fd);
  uint64_t object_flags=0; std::ifstream(path(n)+".flags") >> object_flags;
  maps[p]={sz,marker,object_flags};*out=p;return 0;
}
extern "C" int ubsmem_shmem_unmap(void* p,size_t sz) {
  if(int rc=event("unmap"))return rc;
  auto it=maps.find(p);if(!active || it==maps.end() || it->second.bytes!=sz)return 6002;
  if(munmap(p,sz))return 6005;
  unlink(it->second.path.c_str());maps.erase(it);return 0;
}
extern "C" int ubsmem_shmem_deallocate(const char* n) {
  if(!active)return 6002;
  if(int rc=event("deallocate"))return rc;
  for(const auto& e:std::filesystem::directory_iterator(root()))
    if(e.path().string().find(path(n)+".mapped.")==0)return 6007;
  if(unlink(path(n).c_str())) return 6003;
  unlink((path(n)+".flags").c_str()); return 0;
}

extern "C" int ubsmem_set_logger_level(int) { return event("logger"); }
extern "C" int ubsmem_shmem_set_ownership(const char*, void* p, size_t sz, int prot) {
  if(int rc=event("ownership"))return rc;
  auto it=maps.find(p);
  if(!active || it==maps.end() || it->second.bytes!=sz || it->second.flags!=UBSM_FLAG_CACHE)return 6002;
  return mprotect(p,sz,prot) ? 6005 : 0;
}
