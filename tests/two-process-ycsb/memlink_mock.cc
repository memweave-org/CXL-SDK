// Test-only Memlink ABI stub. This maps a local file; it is NOT UB hardware.
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
extern "C" void* MemlinkMemShmMmap(void* base, size_t size, int prot, int flags,
                                   const char* name, off_t offset, int* error) {
  std::fprintf(stderr,"MEMLINK_TEST_STUB local-file mapping (not UB hardware)\n");
  int fd=open(name,O_RDWR);
  if(fd<0) { *error=errno; return MAP_FAILED; }
  void* p=mmap(base,size,prot,flags|MAP_FIXED_NOREPLACE,fd,offset);
  *error=p==MAP_FAILED?errno:0; close(fd); return p;
}
extern "C" int MemlinkMemShmUnmmap(void* base,size_t size,int* error) {
  int rc=munmap(base,size); *error=rc?errno:0; return rc;
}
