#ifdef SHERMAN_DMA
#define SHERMAN_READ read_dma
#define SHERMAN_WRITE write_dma
#elif defined(SHERMAN_UC)
#define SHERMAN_READ read_plain
#define SHERMAN_WRITE write_plain
#else
#define SHERMAN_READ read_nt
#define SHERMAN_WRITE write_nt
#endif
#include "DSM.h"

#include <algorithm>



#include "utils/atomic_variable.h"

thread_local int DSM::thread_id = -1;
thread_local char *DSM::rdma_buffer = nullptr;
thread_local RdmaBuffer DSM::rbuf[define::kMaxCoro];
thread_local uint64_t DSM::thread_tag = 0;

DSM *DSM::getInstance(const DSMConfig &conf) {
  static DSM *dsm = nullptr;
  static WRLock lock;

  lock.wLock();
  if (!dsm) {
    dsm = new DSM(conf);
  } else {
  }
  lock.wUnlock();

  return dsm;
}

DSM::DSM(const DSMConfig &conf)
    : conf(conf), appID(0), cache(conf.cacheConfig) {}

DSM::~DSM() {}

void DSM::registerThread() {
  if (thread_id != -1) return;

  thread_id = appID.fetch_add(1);
  if(thread_id >= MAX_APP_THREAD) throw std::runtime_error("Sherman thread capacity exceeded");
  thread_tag = thread_id + (((uint64_t)this->getMyNodeID()) << 32) + 1;

  rdma_buffer = (char *)cache.data + thread_id * 12 * define::MB;

  for (int i = 0; i < define::kMaxCoro; ++i) {
    rbuf[i].set_buffer(rdma_buffer + i * define::kPerCoroRdmaBuf);
  }
}

void DSM::read(char* b, GlobalAddress a, size_t n, bool, CoroContext* c) {
  if(c) throw std::runtime_error("CXL coroutine transport unsupported");
  cxl_transport::SHERMAN_READ(b,(void*)a.val,n);
}
void DSM::read_sync(char* b, GlobalAddress a, size_t n, CoroContext* c) { read(b,a,n,true,c); }
void DSM::write(const char* b, GlobalAddress a, size_t n, bool, CoroContext* c) {
  if(c) throw std::runtime_error("CXL coroutine transport unsupported");
  cxl_transport::SHERMAN_WRITE((void*)a.val,b,n);
}
void DSM::write_sync(const char* b, GlobalAddress a, size_t n, CoroContext* c) { write(b,a,n,true,c); }
void DSM::write_batch(CXLOpRegion* rs,int n,bool,CoroContext* c) {
  if(c) throw std::runtime_error("CXL coroutine transport unsupported");
  for(int i=0;i<n;++i) cxl_transport::SHERMAN_WRITE((void*)rs[i].dest,(void*)rs[i].source,rs[i].size);
}
void DSM::write_batch_sync(CXLOpRegion* rs,int n,CoroContext* c) { write_batch(rs,n,true,c); }
void DSM::write_dm(const char* b,GlobalAddress a,size_t n,bool signal,CoroContext* c) { write(b,a,n,signal,c); }
void DSM::write_dm_sync(const char* b,GlobalAddress a,size_t n,CoroContext* c) { write(b,a,n,true,c); }
