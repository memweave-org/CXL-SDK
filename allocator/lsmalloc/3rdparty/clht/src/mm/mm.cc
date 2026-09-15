#include "shm/mm.h"

#include "utils/config.h"
#include "utils/sim_id.h"

#include <cstdint>
#include <fcntl.h>
#include <numa.h>
#include <numaif.h>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

MemoryManager cacheable;
#ifdef ENABLE_UNCACHE_MEM
MemoryManager uncacheable;
#endif

SystemMemoryMmapper::SystemMemoryMmapper(const char* path) : mmap_base(nullptr) {
    strcpy(this->path, path);
    fd = open(path, O_RDWR);
    if (fd < 0) {
        std::string error_msg = "Failed to open mmap device. " + std::string(path);
        throw std::runtime_error(error_msg);
    }
}

void SystemMemoryMmapper::allocate(void* b, size_t s) {
    if (mmap_base != nullptr) {
        throw std::runtime_error("Memory already allocated.");
    }
    mmap_size = s;
#ifdef ALLOC_REMOTE_MEM
    struct bitmask* nodemask = numa_allocate_nodemask();
    numa_bitmask_setbit(nodemask, SHM_NODE);
    numa_set_membind(nodemask);
    numa_set_strict(1);
#endif
    if (fd == -1) {
        mmap_base = mmap(0, mmap_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    } else {
        mmap_base = mmap(b, mmap_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    }
    if (mmap_base == MAP_FAILED) {
        throw std::runtime_error("Failed to mmap.");
    }
#ifdef ALLOC_REMOTE_MEM
    if (mbind(mmap_base, mmap_size, MPOL_BIND, nodemask->maskp, nodemask->size + 1, 0) != 0) {
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

CXLSystemMemoryMmapper::CXLSystemMemoryMmapper(const char* path, int num_machines, int machine_no)
    : SystemMemoryMmapper(path), machine_no(machine_no), num_machines(num_machines) {}

void CXLSystemMemoryMmapper::allocate(void* base, size_t size) {
    this->mmap_size = size;
    this->mmap_base = mmap(base, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (this->mmap_base == MAP_FAILED) {
        throw std::runtime_error("Failed to mmap.");
    }
    per_machine_size = size / num_machines;
    per_machine_base = (void*)((uintptr_t)this->mmap_base + machine_no * per_machine_size);
}

MemoryManager::MemoryManager() : memkind_pool(nullptr), base(nullptr), size(0) {};

MemoryManager::MemoryManager(SystemMemoryMmapper* allocator, void* b, size_t s)
    : allocator(allocator) {
    allocator->allocate(b, s);
    base = allocator->get_mempool_base();
    size = allocator->get_mempool_size();
    if (memkind_create_fixed(base, size, &memkind_pool) != 0) {
        throw std::runtime_error("Failed to create memkind pool.");
    }
}

MemoryManager::MemoryManager(MemoryManager&& other) noexcept
    : memkind_pool(other.memkind_pool),
      base(other.base),
      size(other.size),
      allocator(other.allocator) {
    other.base = nullptr;
    other.size = 0;
    other.memkind_pool = nullptr;
    other.allocator = nullptr;
}

MemoryManager& MemoryManager::operator=(MemoryManager&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (memkind_pool != nullptr) {
        memkind_destroy_kind(memkind_pool);
    }

    base = other.base;
    size = other.size;
    memkind_pool = other.memkind_pool;
    allocator = other.allocator;

    other.base = nullptr;
    other.size = 0;
    other.memkind_pool = nullptr;
    other.allocator = nullptr;

    return *this;
}

MemoryManager::~MemoryManager() {
    if (memkind_pool != nullptr)
        memkind_destroy_kind(memkind_pool);
    delete allocator;
}

void* MemoryManager::malloc(size_t size) {
#ifdef USE_CXL
    return memkind_malloc(memkind_pool, size);
#else
    return ::malloc(size);
#endif
}

int MemoryManager::posix_memalign(void** memptr, size_t alignment, size_t size) {
#ifdef USE_CXL
    return memkind_posix_memalign(memkind_pool, memptr, alignment, size);
#else
    return ::posix_memalign(memptr, alignment, size);
#endif
}

int MemoryManager::clalign(void** memptr, size_t size) {
#ifdef USE_CXL
    return memkind_posix_memalign(memkind_pool, memptr, CACHE_LINE_SIZE, size);
#else
    return ::posix_memalign(memptr, CACHE_LINE_SIZE, size);
#endif
}

void MemoryManager::free(void* ptr) {
#ifdef USE_CXL
    return memkind_free(memkind_pool, ptr);
#else
    return ::free(ptr);
#endif
}

void init_cacheable_allocator(const char* shm_path, void* base, size_t size) {
    auto allocator = new SystemMemoryMmapper(shm_path);
    cacheable = MemoryManager(allocator, base, size);
}

void init_cxl_cacheable_allocator(const char* shm_path, void* base, size_t size) {
    auto allocator = new CXLSystemMemoryMmapper(shm_path, SimThreadInfo::worker_machine_count,
                                                SimThreadInfo::worker_machine_id);
    cacheable = MemoryManager(allocator, base, size);
}

#ifdef ENABLE_UNCACHE_MEM
void init_uncacheable_allocator(const char* shm_path, void* base, size_t size) {
    // auto uncache_allocator = new SystemMemoryAllocator();
    auto uncache_allocator = new SystemMemoryAllocator(shm_path);
    uncacheable = MemoryManager(uncache_allocator, base, size);
    std::cout << "initialize uncache memory finished" << std::endl;
}
#endif