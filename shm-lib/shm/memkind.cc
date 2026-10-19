// Legacy message-queue arena; matches the architecture guard in mempool.cc.
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64) || defined(__aarch64__)
#include "shm/memkind.h"

#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <iostream>

#include "connection/establish.h"
#include "msg/msg_queue.h"
#include "utils/config.h"
#include "utils/sim_id.h"

size_t SHM_SIZE = (size_t)16 * 1024 * 1024 * 1024;
size_t SHM_TOTAL_SIZE = SHM_SIZE * NUM_CLIENTS;
void* GLOBAL_BASE = (reinterpret_cast<void*>(0xcaffe0000000));
void* LOCAL_BASE;
void* LOCAL_BORDER;
memkind_t memkind_pool = nullptr;

size_t UNCACHE_SIZE = (size_t)128 * 1024 * 1024;
void* LOCAL_UNCACHE_BASE;
static char UNCACHE_DEV[PATH_MAX] = "/dev/uncached_mem_dev";

size_t QUEUE_SIZE = NUM_CLIENTS * (NUM_CLIENTS - 1) * sizeof(msg_queue_t);
size_t TOTAL_QUEUE_SIZE = MSG_TYPE_NUM * QUEUE_SIZE;
void* QUEUE_BASE = (char*)GLOBAL_BASE + SHM_TOTAL_SIZE;

// static char shm_path[PATH_MAX] =
// "/sys/bus/pci/devices/0000:00:04.0/resource2";
static char shm_path[PATH_MAX] = "/dev/shm/cxl";

// void setup_shm_args(std::string inifile) {

// }

// void init_cacheable_allocator(int index) {
//   auto allocator = new SystemMemoryMmapper(shm_path);
//   cacheable = MemoryManager(allocator, GLOBAL_BASE, SHM_SIZE);
// }

// void init_uncacheable_allocator() {
//   #ifdef ENABLE_UNCACHE_MEM
//   // auto uncache_allocator = new SystemMemoryAllocator();
//   auto uncache_allocator = new SystemMemoryAllocator(UNCACHE_DEV);
//   uncacheable = std::move(MemoryManager(uncache_allocator, 0, UNCACHE_SIZE));
//   std::cout << "initialize uncache memory finished" << std::endl;
// #endif
// }

#endif
