#include "utils/platform.h"
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <iostream>
#include <cstdlib>

using Pair = unsigned __int128;
static Pair packed(uint64_t n) { return Pair(n) | (Pair(~n) << 64); }
static void require(bool ok) { if (!ok) std::abort(); }
int main() {
  auto* word=static_cast<Pair*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0));
  require(word!=MAP_FAILED);
  *word=packed(0);
  auto work=[&] {
    std::vector<std::thread> workers;
    for(int t=0;t<2;++t) workers.emplace_back([&] {
      Pair expected=shm_platform::compare_exchange_128(word,0,0);
      for(int i=0;i<10000;++i) {
        for(;;) {
          uint64_t value=uint64_t(expected);
          require(uint64_t(expected>>64)==~value);
          Pair old=shm_platform::compare_exchange_128(word,expected,packed(value+1));
          if(old==expected) { expected=packed(value+1); break; }
          expected=old;
          shm_platform::pause();
        }
      }
    });
    for(auto& t:workers)t.join();
  };
  pid_t child=fork(); require(child>=0);
  if(!child) { work(); _exit(0); }
  work();
  int status=0; require(waitpid(child,&status,0)==child);
  require(WIFEXITED(status)&&WEXITSTATUS(status)==0);
  require(shm_platform::compare_exchange_128(word,0,0)==packed(40000));
  // Wrong expected value must leave the shared pair unchanged.
  require(shm_platform::compare_exchange_128(word,packed(7),packed(8))==packed(40000));
  require(*word==packed(40000));
  alignas(64) char src[128], dst[128];
  for(unsigned i=0;i<sizeof(src);++i)src[i]=char(i);
  memcpy_nt_write(dst,src,sizeof(src));
  shm_platform::store_fence();
  require(std::memcmp(src,dst,sizeof(src))==0);
  std::memset(dst,0,sizeof(dst));
  memcpy_nt_read(dst,src,sizeof(src));
  require(std::memcmp(src,dst,sizeof(src))==0);
  munmap(word,4096);
  std::cout << "PASS: 128-bit CAS, 2 processes x 2 threads, 40000 increments, paired-word invariant, copy/fences\n";
}
