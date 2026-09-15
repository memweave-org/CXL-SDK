#include "shm/mm.h"
#include "shm/memkind.h"

#include "utils/config.h"
#include <fcntl.h>
#include <numa.h>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include "utils/sim_id.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <numaif.h>

#ifdef SHM_LIB_WITH_CXLALLOC
#include "cxlalloc.h"
#endif

MemoryManager cacheable;
#ifdef ENABLE_UNCACHE_MEM
MemoryManager uncacheable;
#endif

#ifdef SHM_LIB_WITH_CXLALLOC
namespace {
std::atomic<uint16_t> g_cxlalloc_thread_cap{1};
std::atomic<uint16_t> g_next_cxlalloc_subthread{1};

// cxlalloc_init_process(..., thread_id=0) already calls cxlalloc_init_thread on the
// calling thread; mark TLS so the first malloc on that thread does not re-assign id.
thread_local bool g_cxlalloc_thread_ready = false;

void mark_cxlalloc_thread_ready_after_process_init() {
  g_cxlalloc_thread_ready = true;
}

void ensure_cxlalloc_thread_for_current() {
  if (!cacheable.is_cxlalloc_backend()) {
    return;
  }
  if (g_cxlalloc_thread_ready) {
    return;
  }
  const uint16_t cap = g_cxlalloc_thread_cap.load(std::memory_order_relaxed);
  uint16_t id = g_next_cxlalloc_subthread.fetch_add(1, std::memory_order_acq_rel);
  if (id >= cap) {
    std::cerr << "[MemoryManager] cxlalloc: exceeded configured thread_count="
              << static_cast<int>(cap) << std::endl;
    std::abort();
  }
  cxlalloc_init_thread(id);
  g_cxlalloc_thread_ready = true;
}
} // namespace
#endif

SystemMemoryMmapper::SystemMemoryMmapper(const char *path) : mmap_base(nullptr) {
  strcpy(this->path, path);
  fd = open(path, O_RDWR);
  if (fd < 0) {
    std::string error_msg = "Failed to open mmap device. " + std::string(path);
    throw std::runtime_error(error_msg);
  }
}

void SystemMemoryMmapper::allocate(void *b, size_t s) {
  if (mmap_base != nullptr) {
    throw std::runtime_error("Memory already allocated.");
  }
  mmap_size = s;
#ifdef ALLOC_REMOTE_MEM
  struct bitmask *nodemask = numa_allocate_nodemask();
  numa_bitmask_setbit(nodemask, SHM_NODE);
  numa_set_membind(nodemask);
  numa_set_strict(1);
#endif
  if (fd == -1) {
    mmap_base = mmap(0, mmap_size, PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  } else {
    // Use MAP_SHARED for multi-process shared memory
    // Use MAP_FIXED if base address is provided to ensure same address space across processes
    int flags = MAP_SHARED;
    if (b != nullptr) {
      flags |= MAP_FIXED;
    }
    mmap_base = mmap(b, mmap_size, PROT_READ | PROT_WRITE, flags, fd, 0);
    
    // Debug: Print actual mmap address
    if (mmap_base != MAP_FAILED) {
      if (mmap_base != b && b != nullptr) {
        std::cout << "[SystemMemoryMmapper] WARNING: mmap returned " << mmap_base 
                  << " instead of requested " << b << std::endl;
      } else {
        std::cout << "[SystemMemoryMmapper] mmap succeeded: base=" << mmap_base 
                  << ", requested=" << b << std::endl;
      }
    }
  }
  if( mmap_base == MAP_FAILED) {
    throw std::runtime_error("Failed to mmap.");
  }
#ifdef ALLOC_REMOTE_MEM
  if (mbind(mmap_base, mmap_size, MPOL_BIND, nodemask->maskp,
            nodemask->size + 1, 0) != 0) {
    throw std::runtime_error("Failed to bind memory to NUMA node.");
  }
  numa_bind(numa_all_nodes_ptr);
  numa_free_nodemask(nodemask);
#endif
  // Load all mmap'ed pages into memory
  memset(mmap_base, 0, mmap_size);
}

void SystemMemoryMmapper::deallocate() {
  if (mmap_base == nullptr) {
    return;
  }
  munmap(mmap_base, mmap_size);
  mmap_base = nullptr;
}

SystemMemoryMmapper::~SystemMemoryMmapper() {
  if (mmap_base != nullptr) {
    deallocate();
  }
  if (fd != -1) {
    close(fd);
  }
}

CXLSystemMemoryMmapper::CXLSystemMemoryMmapper(const char *path, int num_machines,
                                               int machine_no)
    : SystemMemoryMmapper(path), machine_no(machine_no),
      num_machines(num_machines) {}

void CXLSystemMemoryMmapper::allocate(void *base, size_t size) {
  this->mmap_size = size;
  this->mmap_base = mmap(base, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (this->mmap_base == MAP_FAILED) {
    throw std::runtime_error("Failed to mmap.");
  }
  per_machine_size = size / num_machines;
  per_machine_base =
      (void *)((uintptr_t)this->mmap_base + machine_no * per_machine_size);
}

MemoryManager::MemoryManager()
    : memkind_pool(nullptr),
      backend_(CacheableAllocatorBackend::Memkind),
      base(nullptr),
      size(0),
      allocator(nullptr) {};

MemoryManager::MemoryManager(CxlallocCacheableTag, size_t reported_size_bytes)
    : memkind_pool(nullptr),
      backend_(CacheableAllocatorBackend::Cxlalloc),
      base(nullptr),
      size(reported_size_bytes),
      allocator(nullptr) {}

MemoryManager::MemoryManager(SystemMemoryMmapper *allocator, void *b, size_t s)
    : memkind_pool(nullptr),
      backend_(CacheableAllocatorBackend::Memkind),
      allocator(allocator) {
  allocator->allocate(b, s);
  base = allocator->get_mempool_base();
  size = allocator->get_mempool_size();
  
  // Debug: Print memkind pool base and size
  std::cout << "[MemoryManager] Creating memkind pool: base=" << base 
            << ", size=" << size << ", requested_base=" << b << std::endl;
  
  // memkind_create_fixed requires the actual mmap base address, not the requested one
  // If mmap failed to use MAP_FIXED, we need to use the actual mmap address
  void* actual_base = allocator->get_mmap_base();
  if (actual_base != base && actual_base != nullptr) {
    std::cout << "[MemoryManager] WARNING: mmap base (" << actual_base 
              << ") differs from requested base (" << b << ")" << std::endl;
    std::cout << "[MemoryManager] Using actual mmap base for memkind pool" << std::endl;
    base = actual_base;
  }
  
  if (memkind_create_fixed(base, size, &memkind_pool) != 0) {
    std::cerr << "[MemoryManager] ERROR: Failed to create memkind pool. base=" 
              << base << ", size=" << size << std::endl;
    std::cerr << "[MemoryManager] memkind error: " << strerror(errno) << std::endl;
    throw std::runtime_error("Failed to create memkind pool.");
  }
  
  std::cout << "[MemoryManager] Successfully created memkind pool at " << base << std::endl;
}

MemoryManager::MemoryManager(MemoryManager &&other) noexcept
    : memkind_pool(other.memkind_pool),
      backend_(other.backend_),
      base(other.base),
      size(other.size),
      allocator(other.allocator) {
  other.base = nullptr;
  other.size = 0;
  other.memkind_pool = nullptr;
  other.allocator = nullptr;
  other.backend_ = CacheableAllocatorBackend::Memkind;
}

MemoryManager &MemoryManager::operator=(MemoryManager &&other) noexcept {
  if (this == &other) {
    return *this;
  }

  if (backend_ == CacheableAllocatorBackend::Memkind) {
    if (memkind_pool != nullptr)
      memkind_destroy_kind(memkind_pool);
    delete allocator;
  }

  base = other.base;
  size = other.size;
  memkind_pool = other.memkind_pool;
  allocator = other.allocator;
  backend_ = other.backend_;

  other.base = nullptr;
  other.size = 0;
  other.memkind_pool = nullptr;
  other.allocator = nullptr;
  other.backend_ = CacheableAllocatorBackend::Memkind;

  return *this;
}

MemoryManager::~MemoryManager() {
  if (backend_ == CacheableAllocatorBackend::Memkind) {
    if (memkind_pool != nullptr)
      memkind_destroy_kind(memkind_pool);
    delete allocator;
  }
}

bool MemoryManager::owns_memkind_pointer(const void *ptr) const {
  if (ptr == nullptr || base == nullptr || size == 0) {
    return false;
  }

  const auto ptr_val = reinterpret_cast<uintptr_t>(ptr);
  const auto base_val = reinterpret_cast<uintptr_t>(base);
  return ptr_val >= base_val && ptr_val < base_val + size;
}

void *MemoryManager::malloc(size_t size) {
  if (backend_ == CacheableAllocatorBackend::Cxlalloc) {
#ifdef SHM_LIB_WITH_CXLALLOC
    ensure_cxlalloc_thread_for_current();
    void *p = cxlalloc_malloc(size);
    if (p == nullptr) {
      return nullptr;
    }
    return p;
#else
    (void)size;
    std::cerr << "[MemoryManager::malloc] cxlalloc backend not compiled in\n";
    return nullptr;
#endif
  }
#ifdef USE_CXL
  if (memkind_pool == nullptr) {
    std::cerr << "[MemoryManager::malloc] ERROR: memkind_pool is nullptr!" << std::endl;
    return ::malloc(size); // Fallback to heap
  }
  void* ptr = memkind_malloc(memkind_pool, size);
  if (ptr == nullptr) {
    std::cerr << "[MemoryManager::malloc] ERROR: memkind_malloc returned nullptr; "
              << "falling back to heap for " << size << " bytes" << std::endl;
    return ::malloc(size); // Fallback to heap
  }
  // Debug: Check if allocated address is in expected range
  uintptr_t ptr_val = reinterpret_cast<uintptr_t>(ptr);
  uintptr_t base_val = reinterpret_cast<uintptr_t>(base);
  if (ptr_val < base_val || ptr_val >= base_val + this->size) {
    std::cerr << "[MemoryManager::malloc] WARNING: Allocated address " << ptr 
              << " is outside memkind pool range [" << base << ", " 
              << (void*)((char*)base + this->size) << "]" << std::endl;
    std::cerr << "[MemoryManager::malloc] ptr_val=" << ptr_val 
              << ", base_val=" << base_val << ", size=" << this->size << std::endl;
  } else {
    // Only print first few allocations to avoid spam
    static int alloc_count = 0;
    if (alloc_count++ < 5) {
      std::cout << "[MemoryManager::malloc] Allocated " << size << " bytes at " << ptr 
                << " (in pool range)" << std::endl;
    }
  }
  return ptr;
#else
  return ::malloc(size);
#endif
}

int MemoryManager::posix_memalign(void **memptr, size_t alignment,
                                  size_t size) {
  if (backend_ == CacheableAllocatorBackend::Cxlalloc) {
#ifdef SHM_LIB_WITH_CXLALLOC
    ensure_cxlalloc_thread_for_current();
    void *p = cxlalloc_memalign(size, alignment);
    if (p == nullptr) {
      return ENOMEM;
    }
    *memptr = p;
    return 0;
#else
    (void)memptr;
    (void)alignment;
    (void)size;
    return ENOMEM;
#endif
  }
#ifdef USE_CXL
  if (memkind_pool != nullptr) {
    int rc = memkind_posix_memalign(memkind_pool, memptr, alignment, size);
    if (rc == 0) {
      return 0;
    }
    std::cerr << "[MemoryManager::posix_memalign] ERROR: memkind_posix_memalign "
              << "failed with " << rc << "; falling back to heap for " << size
              << " bytes" << std::endl;
  } else {
    std::cerr << "[MemoryManager::posix_memalign] ERROR: memkind_pool is nullptr; "
              << "falling back to heap for " << size << " bytes" << std::endl;
  }
  return ::posix_memalign(memptr, alignment, size);
#else
  return ::posix_memalign(memptr, alignment, size);
#endif
}

int MemoryManager::clalign(void **memptr, size_t size) {
  if (backend_ == CacheableAllocatorBackend::Cxlalloc) {
#ifdef SHM_LIB_WITH_CXLALLOC
    ensure_cxlalloc_thread_for_current();
    void *p = cxlalloc_memalign(size, CACHE_LINE_SIZE);
    if (p == nullptr) {
      return ENOMEM;
    }
    *memptr = p;
    return 0;
#else
    (void)memptr;
    (void)size;
    return ENOMEM;
#endif
  }
#ifdef USE_CXL
  if (memkind_pool != nullptr) {
    int rc = memkind_posix_memalign(memkind_pool, memptr, CACHE_LINE_SIZE, size);
    if (rc == 0) {
      return 0;
    }
    std::cerr << "[MemoryManager::clalign] ERROR: memkind_posix_memalign "
              << "failed with " << rc << "; falling back to heap for " << size
              << " bytes" << std::endl;
  } else {
    std::cerr << "[MemoryManager::clalign] ERROR: memkind_pool is nullptr; "
              << "falling back to heap for " << size << " bytes" << std::endl;
  }
  return ::posix_memalign(memptr, CACHE_LINE_SIZE, size);
#else
  return ::posix_memalign(memptr, CACHE_LINE_SIZE, size);
#endif
}

void MemoryManager::free(void *ptr) {
  if (ptr == nullptr) {
    return;
  }

  if (backend_ == CacheableAllocatorBackend::Cxlalloc) {
#ifdef SHM_LIB_WITH_CXLALLOC
    ensure_cxlalloc_thread_for_current();
    cxlalloc_free(ptr);
#endif
    return;
  }
#ifdef USE_CXL
  if (memkind_pool != nullptr && owns_memkind_pointer(ptr)) {
    return memkind_free(memkind_pool, ptr);
  }
  return ::free(ptr);
#else
  return ::free(ptr);
#endif
}

void init_cacheable_allocator(const char *shm_path, void *base, size_t size) {
  auto allocator = new SystemMemoryMmapper(shm_path);
  cacheable = MemoryManager(allocator, base, size);
  memkind_pool = cacheable.memkind_pool;
}

void init_cxl_cacheable_allocator(const char *shm_path, void *base,
                                  size_t size) {
  auto allocator =
      new CXLSystemMemoryMmapper(shm_path, SimThreadInfo::worker_machine_count,
                                 SimThreadInfo::worker_machine_id);
  cacheable = MemoryManager(allocator, base, size);
  memkind_pool = cacheable.memkind_pool;
}

void init_cacheable_allocator_unified(const CacheableInitParams &p) {
  if (p.backend == CacheableAllocatorBackend::Cxlalloc) {
#ifndef SHM_LIB_WITH_CXLALLOC
    throw std::runtime_error(
        "allocator_backend=cxlalloc but shm-lib was built without "
        "WITH_CXLALLOC (prebuild or build allocator/cxlalloc/cxlalloc-static)");
#endif
    if (p.mem_type_cxl && p.worker_machine_count > 1) {
      throw std::runtime_error(
          "allocator_backend=cxlalloc does not support multi-machine "
          "mem_type=cxl (worker_machine_count>1)");
    }
    if (p.device_path == nullptr) {
      throw std::runtime_error("init_cacheable_allocator_unified: device_path is null");
    }
    if (p.thread_count == 0) {
      throw std::runtime_error(
          "init_cacheable_allocator_unified: thread_count must be > 0 for cxlalloc");
    }
#ifdef SHM_LIB_WITH_CXLALLOC
    g_cxlalloc_thread_cap.store(p.thread_count, std::memory_order_relaxed);
    g_next_cxlalloc_subthread.store(1, std::memory_order_relaxed);
    cxlalloc_init_process(p.device_path, p.cxlalloc_heap_numa, "mmap",
                          p.size_bytes, p.thread_count, 0);
    mark_cxlalloc_thread_ready_after_process_init();
    std::cerr << "[MemoryManager] cacheable allocator: cxlalloc (threads="
              << p.thread_count << ", heap_numa="
              << static_cast<int>(p.cxlalloc_heap_numa)
              << ", device=" << p.device_path << ")\n";
    cacheable = MemoryManager(CxlallocCacheableTag{}, p.size_bytes);
    memkind_pool = nullptr;
#endif
    return;
  }

  SystemMemoryMmapper *mapper = nullptr;
  if (p.mem_type_cxl) {
    mapper = new CXLSystemMemoryMmapper(p.device_path, p.worker_machine_count,
                                         p.worker_machine_id);
  } else {
    mapper = new SystemMemoryMmapper(p.device_path);
  }
  cacheable = MemoryManager(mapper, p.mmap_base, p.size_bytes);
  memkind_pool = cacheable.memkind_pool;
}

#ifdef ENABLE_UNCACHE_MEM
void init_uncacheable_allocator(const char *shm_path, void *base,
                                size_t size) {
  auto uncache_allocator = new SystemMemoryAllocator(shm_path);
  uncacheable = MemoryManager(uncache_allocator, base, size);
  std::cout << "initialize uncache memory finished" << std::endl;
}
#endif
