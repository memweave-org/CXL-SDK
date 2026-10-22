#include "shm/ub.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#ifdef SHM_LIB_WITH_UB
extern "C" {
#include "shm/memlink_client.h"
}
#elif defined(SHM_LIB_WITH_UBSM)
#include <ubs_mem.h>
#include <ubs_mem_def.h>
#endif

namespace {
void checked(int rc, const char* operation) {
  if (rc) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(rc));
}
std::mutex api_mutex;
std::map<void*, size_t> mappings;
#ifdef SHM_LIB_WITH_UBSM
bool initialized = false;
void initialize() {
  if (initialized) return;
  ubsmem_options_t options{};
  checked(ubsmem_init_attributes(&options), "ubsmem_init_attributes");
  checked(ubsmem_initialize(&options), "ubsmem_initialize");
  initialized = true;
}
void finalize() {
  if (initialized && mappings.empty()) {
    checked(ubsmem_finalize(), "ubsmem_finalize");
    initialized = false;
  }
}
void validate_name(const char* name) {
  if (!name || !*name || std::strlen(name) > 48)
    throw std::runtime_error("UBSM object name must contain 1..48 characters");
}
#endif
}

const char* ub_api_name() {
#ifdef SHM_LIB_WITH_UBSM
  return "ubsm";
#elif defined(SHM_LIB_WITH_UB)
  return "memlink";
#else
  return "none";
#endif
}

void* map_ub_memory(const char* name, void* base, size_t size) {
  std::lock_guard<std::mutex> guard(api_mutex);
  void* p = nullptr;
#ifdef SHM_LIB_WITH_UBSM
  validate_name(name);
  initialize();
  const int rc = ubsmem_shmem_map(base, size, PROT_READ | PROT_WRITE,
      MAP_SHARED | (base ? MAP_FIXED_NOREPLACE : 0), name, 0, &p);
  if (rc) {
    finalize();
    checked(rc, "ubsmem_shmem_map");
  }
  if (!p || p == MAP_FAILED || (base && p != base)) {
    if (p && p != MAP_FAILED) checked(ubsmem_shmem_unmap(p, size), "unmap unexpected address");
    finalize();
    throw std::runtime_error("UBSM did not map the requested fixed address");
  }
#elif defined(SHM_LIB_WITH_UB)
  int error = 0;
  p = MemlinkMemShmMmap(base, size, PROT_READ | PROT_WRITE, MAP_SHARED, name, 0, &error);
  checked(error, "MemlinkMemShmMmap");
  if (!p || p == MAP_FAILED || (base && p != base)) {
    if (p && p != MAP_FAILED) {
      MemlinkMemShmUnmmap(p, size, &error);
      checked(error, "Memlink unmap unexpected address");
    }
    throw std::runtime_error("Memlink did not map the requested fixed address");
  }
#else
  throw std::runtime_error("UB is disabled; configure global UB_API=memlink or ubsm");
#endif
  mappings.emplace(p, size);
  std::cout << "UB_MAPPING api=" << ub_api_name() << " name=" << name
            << " address=" << p << " bytes=" << size << std::endl;
  return p;
}

void unmap_ub_memory(void* base, size_t size) {
  std::lock_guard<std::mutex> guard(api_mutex);
  auto it = mappings.find(base);
  if (it == mappings.end() || it->second != size)
    throw std::runtime_error("UB unmap must use the exact original address and length");
#ifdef SHM_LIB_WITH_UBSM
  checked(ubsmem_shmem_unmap(base, size), "ubsmem_shmem_unmap");
#elif defined(SHM_LIB_WITH_UB)
  int error = 0;
  MemlinkMemShmUnmmap(base, size, &error);
  checked(error, "MemlinkMemShmUnmmap");
#endif
  mappings.erase(it);
#ifdef SHM_LIB_WITH_UBSM
  finalize();
#endif
}

void create_ub_memory(const char* name, size_t size, const char* host, uint32_t numa) {
  std::lock_guard<std::mutex> guard(api_mutex);
#ifdef SHM_LIB_WITH_UBSM
  validate_name(name);
  ubs_mem_provider_t provider{};
  if (!host || !*host || std::strlen(host) >= sizeof(provider.host_name))
    throw std::runtime_error("A valid UBSM provider hostname is required");
  if (!size || size % (4ul << 20))
    throw std::runtime_error("UBSM size must be a positive multiple of 4 MiB");
  std::strcpy(provider.host_name, host);
  provider.socket_id = provider.port_id = UINT32_MAX;
  provider.numa_id = numa;
  initialize();
  const int rc = ubsmem_shmem_allocate_with_provider(&provider, name, size, 0660,
      UBSM_FLAG_ONLY_IMPORT_NONCACHE | UBSM_FLAG_WR_DELAY_COMP);
  if (!rc) std::cout << "UB_OBJECT_CREATED name=" << name
                     << " requested_provider=" << host << " numa=" << numa
                     << " policy=only-import-noncache" << std::endl;
  finalize();
  checked(rc, "ubsmem_shmem_allocate_with_provider");
#else
  throw std::runtime_error("Automatic object creation requires UB_API=ubsm");
#endif
}

void destroy_ub_memory(const char* name) {
  std::lock_guard<std::mutex> guard(api_mutex);
#ifdef SHM_LIB_WITH_UBSM
  validate_name(name);
  if (!mappings.empty()) throw std::runtime_error("Cannot destroy with local UB mappings still active");
  initialize();
  const int rc = ubsmem_shmem_deallocate(name);
  finalize();
  checked(rc, "ubsmem_shmem_deallocate");
  std::cout << "UB_OBJECT_DESTROYED name=" << name << std::endl;
#else
  throw std::runtime_error("Automatic object destruction requires UB_API=ubsm");
#endif
}
