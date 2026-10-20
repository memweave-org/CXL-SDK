#include "memory.h"
#include "utils/atomic_variable.h"
#include <sys/mman.h>
#include <numaif.h>
#include <unistd.h>
#include "nt_copy.h"
#include "../../drivers/uncached_ram/uncached_ram_uapi.h"
#include <sys/ioctl.h>
#include <cerrno>
#include <string>
#include "shm/mm.h"
#ifdef QUEUE_SIM_CAS
#include "utils/atomic_queued_variable.h"
#endif
#include <cstring>
#include <atomic>
#include "utils/platform.h"
#ifdef __x86_64__
#include <immintrin.h>
#endif
#include <stdexcept>
#include <vector>
#include <algorithm>
namespace cxl_transport {
namespace {
// Sequence counters validate copies across packed, multi-cache-line writes.
// TWO_PROCESS_SHARED_COPY places these counters in the shared arena via
// configure_shared_sequences(); the legacy single-process path uses local storage.
struct alignas(64) CopySequence {
#ifdef TWO_PROCESS_SHARED_COPY
  nt<uint64_t> value{0};
#else
  std::atomic<uint64_t> value{0};
#endif
};
CopySequence local_copy_sequences[65536];
CopySequence* copy_sequences = local_copy_sequences;
std::vector<size_t> copy_slots(const void* p, size_t n) {
  std::vector<size_t> slots;
  if (!n) return slots;
  auto first=uintptr_t(p)>>12, last=(uintptr_t(p)+n-1)>>12;
  for(auto page=first;page<=last;++page) slots.push_back(page%65536);
  std::sort(slots.begin(),slots.end());
  slots.erase(std::unique(slots.begin(),slots.end()),slots.end());
  return slots;
}
struct CopyWriteGuard {
  std::vector<size_t> slots;
  CopyWriteGuard(void* p,size_t n):slots(copy_slots(p,n)) {
    for(auto i:slots) {
      auto& seq=copy_sequences[i].value;
      auto v=seq.load();
      while((v&1) || !seq.compare_exchange_strong(v,v+1)) {
        shm_platform::pause(); v=seq.load();
      }
    }
  }
  ~CopyWriteGuard() { for(auto i:slots) copy_sequences[i].value.fetch_add(1); }
};
}

// Streaming stores require aligned destinations. Short/unaligned fragments
// use scalar copies; callers must provide non-overlapping ranges. atomic_pairs
// preserves Deft's 16-byte publication unit while still streaming the output.
void nt_memcpy(void* dst, const void* src, size_t n, bool atomic_pairs) {
#ifdef SHM_LIB_WITH_UBSM
  if (atomic_pairs) throw std::runtime_error("UBSM does not support 128-bit snapshots");
  auto* out = static_cast<volatile unsigned char*>(dst);
  auto* in = static_cast<const volatile unsigned char*>(src);
  for (size_t i=0; i<n; ++i) out[i]=in[i];
  shm_platform::full_fence();
#elif defined(__x86_64__)
  auto* out = static_cast<unsigned char*>(dst);
  auto* in = static_cast<const unsigned char*>(src);
  size_t i = 0;
  if (atomic_pairs && (uintptr_t(src) & 15) == 0) {
    for (; i + 16 <= n; i += 16) {
      auto pair = shm_platform::compare_exchange_128(
          (unsigned __int128*)(in + i), 0, 0);
      __m128i value;
      memcpy(&value, &pair, sizeof(value));
      if ((uintptr_t(out + i) & 15) == 0)
        _mm_stream_si128(reinterpret_cast<__m128i*>(out + i), value);
      else
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), value);
    }
  } else {
    for (; i < n && (uintptr_t(out + i) & 15); ++i)
      out[i] = in[i];
    for (; i + 16 <= n; i += 16) {
      auto value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + i));
      _mm_stream_si128(reinterpret_cast<__m128i*>(out + i), value);
    }
  }
  for (; i < n; ++i) out[i] = in[i];
  // The caller may immediately consume the scratch buffer or publish a version.
  shm_platform::store_fence();
#elif defined(__aarch64__)
  if (atomic_pairs && (uintptr_t(src) & 15) == 0) {
    size_t i=0;
    for (; i+16<=n; i+=16) {
      auto pair=shm_platform::compare_exchange_128(
          reinterpret_cast<unsigned __int128*>(const_cast<char*>(static_cast<const char*>(src))+i), 0, 0);
      std::memcpy(static_cast<char*>(dst)+i,&pair,16);
    }
    std::memcpy(static_cast<char*>(dst)+i,static_cast<const char*>(src)+i,n-i);
  } else {
    std::memcpy(dst,src,n);
  }
  shm_platform::store_fence();
#else
#error Unsupported architecture
#endif
}

size_t shared_sequences_bytes() { return sizeof(local_copy_sequences); }
void configure_shared_sequences(void* p) { copy_sequences = static_cast<CopySequence*>(p); }

void* allocate(size_t n) {
  void* p = nullptr;
  if (cacheable.posix_memalign(&p, 64, (n+63)&~size_t(63)) || !p) throw std::bad_alloc();
  return p;
}
void release(void* p) { cacheable.free(p); }
namespace {
int dma_fd = -1;
uintptr_t dma_base = 0;
size_t dma_bytes = 0;
void dma_transfer(void* local, const void* remote, size_t bytes, bool writing) {
  uintptr_t addr = reinterpret_cast<uintptr_t>(remote);
  if (dma_fd < 0 || addr < dma_base || addr - dma_base > dma_bytes ||
      bytes > dma_bytes - (addr - dma_base))
    throw std::runtime_error("DMA copy outside configured UC arena");
  size_t offset = addr - dma_base;
  auto* buffer = static_cast<unsigned char*>(local);
  while (bytes) {
    const size_t n = std::min({bytes, size_t(4096), size_t(2097152 - offset % 2097152)});
    uncached_ram_dma_copy req{offset, reinterpret_cast<uintptr_t>(buffer),
                             uint32_t(n), uint32_t(writing ? UNCACHED_RAM_DMA_WRITE : UNCACHED_RAM_DMA_READ)};
    if (ioctl(dma_fd, UNCACHED_RAM_DMA_COPY, &req))
      throw std::runtime_error(std::string("UC DMA copy: ") + std::strerror(errno));
    offset += n; buffer += n; bytes -= n;
  }
}
}
void* allocate_wb_nt_locks(size_t bytes,int node) {
  if(node<0 || node>=64 || bytes%sizeof(nt<uint64_t>)) throw std::runtime_error("invalid WB lock layout");
  auto* p=static_cast<char*>(mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
  if(p==MAP_FAILED) throw std::bad_alloc();
  unsigned long mask=1ul<<node;
  if(mbind(p,bytes,MPOL_BIND,&mask,64,0)) {
    munmap(p,bytes); throw std::runtime_error("WB lock binding failed");
  }
  for(size_t i=0;i<bytes;i+=sizeof(nt<uint64_t>)) new(p+i) nt<uint64_t>(0);
  return p;
}
uint64_t nt_lock_load(void* p) { return static_cast<nt<uint64_t>*>(p)->load(); }
void nt_lock_store(void* p,uint64_t v) { static_cast<nt<uint64_t>*>(p)->store(v); }
bool nt_lock_cas(void* p,uint64_t& e,uint64_t v) { return static_cast<nt<uint64_t>*>(p)->compare_exchange_strong(e,v); }
void configure_dma(int fd, void* arena, size_t bytes, int engine_node) {
  if (dma_fd >= 0) throw std::runtime_error("DMA already configured");
  if (ioctl(fd, UNCACHED_RAM_ENABLE_DMA, &engine_node))
    throw std::runtime_error(std::string("DSA channel unavailable: ") + std::strerror(errno));
  dma_base = reinterpret_cast<uintptr_t>(arena); dma_bytes = bytes; dma_fd = fd;
}
// Readers invalidate shared source lines; writers publish before lock release.
// The remote-CPU/DMA style optimistic snapshots rely on Deft/Sherman versions.
template<bool nt_only, bool plain_copy = false, bool dma_copy = false>
static void read_snapshot(void* dst, const void* src, size_t n, bool atomic_pairs) {
#ifdef NO_CC
  if constexpr (!nt_only) {
  clflush(src,n);
  shm_platform::full_fence();
  }
#endif
  const auto slots=copy_slots(src,n);
  std::vector<uint64_t> versions(slots.size());
retry_snapshot:
  for(size_t j=0;j<slots.size();++j) {
    auto v=copy_sequences[slots[j]].value.load();
    if(v&1) { shm_platform::pause(); goto retry_snapshot; }
    versions[j]=v;
  }
  if constexpr (dma_copy) dma_transfer(dst, src, n, false);
  else if constexpr (plain_copy) std::memcpy(dst, src, n);
  else if constexpr (nt_only) nt_copy::stream_copy(dst, src, n);
  else nt_memcpy(dst, src, n, atomic_pairs);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  for(size_t j=0;j<slots.size();++j)
    if(copy_sequences[slots[j]].value.load()!=versions[j]) goto retry_snapshot;
}
void read(void* dst, const void* src, size_t n, bool atomic_pairs) {
  read_snapshot<false>(dst, src, n, atomic_pairs);
}
void read_projected(const void* src, size_t n, void* output,
                    void (*project)(const void*, void*)) {
  const auto slots = copy_slots(src, n);
  std::vector<uint64_t> versions(slots.size());
  for (;;) {
    bool busy = false;
    for (size_t j = 0; j < slots.size(); ++j) {
      versions[j] = copy_sequences[slots[j]].value.load();
      busy |= versions[j] & 1;
    }
    if (busy) { shm_platform::pause(); continue; }
    project(src, output);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    bool valid = true;
    for (size_t j = 0; j < slots.size(); ++j)
      valid &= copy_sequences[slots[j]].value.load() == versions[j];
    if (valid) return;
  }
}
void read_nt(void* dst, const void* src, size_t n) {
  read_snapshot<true>(dst, src, n, false);
}
void read_dma(void* dst, const void* src, size_t n) {
  read_snapshot<true, false, true>(dst, src, n, false);
}
void write_dma(void* dst, const void* src, size_t n) {
  CopyWriteGuard snapshot_guard(dst,n);
  dma_transfer(const_cast<void*>(src), dst, n, true);
  std::atomic_thread_fence(std::memory_order_seq_cst);
}
void read_plain(void* dst, const void* src, size_t n) {
  read_snapshot<true, true>(dst, src, n, false);
}
void write_plain(void* dst, const void* src, size_t n) {
  CopyWriteGuard snapshot_guard(dst,n);
  std::memcpy(dst, src, n);
  std::atomic_thread_fence(std::memory_order_seq_cst);
}
void write_nt(void* dst, const void* src, size_t n) {
  CopyWriteGuard snapshot_guard(dst,n);
  nt_copy::stream_copy(dst, src, n);
}
void write(void* dst, const void* src, size_t n) {
  CopyWriteGuard snapshot_guard(dst,n);
  nt_memcpy(dst, src, n);
#ifdef NO_CC
  clwb(dst,n);
  shm_platform::store_fence();
#else
  std::atomic_thread_fence(std::memory_order_release);
#endif
}
uint64_t load64(const void* p) {
#ifdef QUEUE_SIM_CAS
  AtomicRequestQueue::pend_load_req(p);
#endif
  return __atomic_load_n((const uint64_t*)p, __ATOMIC_ACQUIRE);
}
void store64(void* p, uint64_t v) {
#ifdef QUEUE_SIM_CAS
  AtomicRequestQueue::pend_store_req(p);
#endif
  __atomic_store_n((uint64_t*)p, v, __ATOMIC_RELEASE);
}
bool cas64(void* p, uint64_t& expected, uint64_t desired) {
  bool ok = __atomic_compare_exchange_n((uint64_t*)p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#ifdef QUEUE_SIM_CAS
  AtomicRequestQueue::pend_cas_req(p);
#endif
  return ok;
}
bool cas64_snapshot(void* p, uint64_t& expected, uint64_t desired) {
  CopyWriteGuard guard(p,8);
  return cas64(p,expected,desired);
}
bool cas128_snapshot(void* p, uint64_t expected[2], const uint64_t desired[2]) {
  CopyWriteGuard guard(p,16);
  return cas128(p,expected,desired);
}
bool cas128(void* p, uint64_t expected[2], const uint64_t desired[2]) {
  if (uintptr_t(p)%16) throw std::runtime_error("unaligned Deft 128-bit CAS");
  using U = unsigned __int128;
  U e = U(expected[0]) | (U(expected[1])<<64);
  U d = U(desired[0]) | (U(desired[1])<<64);
  U old = shm_platform::compare_exchange_128((U*)p,e,d);
  expected[0] = uint64_t(old); expected[1] = uint64_t(old>>64);
#ifdef QUEUE_SIM_CAS
  AtomicRequestQueue::pend_cas_req(p);
#endif
  return old == e;
}
}
