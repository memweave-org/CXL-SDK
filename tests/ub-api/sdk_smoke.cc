// Standalone vendor SDK test: no shm-lib, allocator, index, or latency simulator.
#include <ubs_mem.h>
#include <ubs_mem_def.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <ctime>

static void check(const char* op, int rc) {
  if (rc) throw std::runtime_error(std::string(op) + " failed: " + std::to_string(rc));
}
template<class F> static int trace(const char* op, F action) {
  std::cout << "BEGIN " << op << std::endl;
  const int rc = action();
  std::cout << "END " << op << " rc=" << rc << std::endl;
  return rc;
}
#define SDK_CALL(op, expression) check(op, trace(op, [&] { return (expression); }))

static void fence() {
#if defined(__aarch64__)
  asm volatile("dmb sy" ::: "memory");
#else
  __sync_synchronize();
#endif
}
int main(int argc, char** argv) {
  bool initialized = false, created = false, owned = false;
  void* address = nullptr;
  size_t bytes = 0;
  std::string name;
  try {
    if (argc < 2) throw std::runtime_error("usage: ub_sdk_smoke init | cache|noncache PROVIDER [SIZE_MB]");
    const std::string mode = argv[1];
    if (mode != "init" && mode != "cache" && mode != "noncache")
      throw std::runtime_error("mode must be init, cache or noncache");
    ubs_mem_provider_t provider{};
    if (mode != "init") {
      if (argc < 3 || !argv[2][0] || std::strlen(argv[2]) >= sizeof(provider.host_name))
        throw std::runtime_error("valid provider UB topology hostname required");
      std::strcpy(provider.host_name, argv[2]);
      provider.socket_id = provider.numa_id = provider.port_id = UINT32_MAX;
      const std::string size_arg = argc > 3 ? argv[3] : "128";
      if (size_arg.empty() || size_arg.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("SIZE_MB must be a positive integer");
      const auto mb = std::stoull(size_arg);
      if (mb < 4 || mb % 4 || mb > std::numeric_limits<size_t>::max() / (1ul << 20))
        throw std::runtime_error("SIZE_MB must be a multiple of 4, at least 4");
      bytes = size_t(mb) << 20;
      name = "sdk-" + mode + "-" + std::to_string(std::time(nullptr)) + "-" + std::to_string(getpid());
    }
    std::cout << "MODE=" << mode << " (real SDK unless explicitly linked to a test stub)" << std::endl;
    // Match the supplied example, but keep debug output for diagnosis.
    SDK_CALL("ubsmem_set_logger_level(debug)", ubsmem_set_logger_level(0));
    ubsmem_options_t options{};
    SDK_CALL("ubsmem_init_attributes", ubsmem_init_attributes(&options));
    SDK_CALL("ubsmem_initialize", ubsmem_initialize(&options));
    initialized = true;
    if (mode != "init") {
      const uint64_t flags = mode == "cache" ? UBSM_FLAG_CACHE :
          UBSM_FLAG_NONCACHE | UBSM_FLAG_WR_DELAY_COMP;
      std::cout << "CREATE name=" << name << " provider=" << provider.host_name
                << " bytes=" << bytes << " flags=" << flags << std::endl;
      SDK_CALL("ubsmem_shmem_allocate_with_provider",
            ubsmem_shmem_allocate_with_provider(&provider, name.c_str(), bytes, 0660, flags));
      created = true;
      SDK_CALL("ubsmem_shmem_map", ubsmem_shmem_map(nullptr, bytes, PROT_READ | PROT_WRITE,
                                                MAP_SHARED, name.c_str(), 0, &address));
      if (!address || address == MAP_FAILED) throw std::runtime_error("invalid mapped address");
      std::cout << "MAPPED address=" << address << std::endl;
      if (mode == "cache") {
        SDK_CALL("ubsmem_shmem_set_ownership(RW)",
              ubsmem_shmem_set_ownership(name.c_str(), address, bytes, PROT_READ | PROT_WRITE));
        owned = true;
      }
      // Scalar volatile accesses avoid compiler-generated vector stores on NC.
      auto* words = static_cast<volatile uint64_t*>(address);
      std::cout << "BEGIN write_4096" << std::endl;
      for (size_t i = 0; i < 512; ++i) words[i] = 0x1234567800000000ULL ^ i;
      std::cout << "END write_4096\nBEGIN fence" << std::endl;
      fence();
      std::cout << "END fence" << std::endl;
      if (mode == "cache")
        SDK_CALL("ubsmem_shmem_set_ownership(READ)",
              ubsmem_shmem_set_ownership(name.c_str(), address, bytes, PROT_READ));
      std::cout << "BEGIN readback_4096" << std::endl;
      for (size_t i = 0; i < 512; ++i)
        if (words[i] != (0x1234567800000000ULL ^ i))
          throw std::runtime_error("readback mismatch at word " + std::to_string(i));
      std::cout << "END readback_4096\nWRITE_READBACK_PASS bytes=4096" << std::endl;
      if (owned) {
        SDK_CALL("ubsmem_shmem_set_ownership(NONE)",
              ubsmem_shmem_set_ownership(name.c_str(), address, bytes, PROT_NONE));
        owned = false;
      }
      SDK_CALL("ubsmem_shmem_unmap", ubsmem_shmem_unmap(address, bytes));
      address = nullptr;
      SDK_CALL("ubsmem_shmem_deallocate", ubsmem_shmem_deallocate(name.c_str()));
      created = false;
    }
    SDK_CALL("ubsmem_finalize", ubsmem_finalize());
    initialized = false;
    std::cout << "PASS mode=" << mode << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << std::endl;
    // Never deallocate a region whose unmap failed. Print every cleanup result.
    if (owned) {
      const int rc = trace("cleanup ownership(NONE)", [&] {
        return ubsmem_shmem_set_ownership(name.c_str(), address, bytes, PROT_NONE); });
      std::cerr << "cleanup ownership(NONE) rc=" << rc << std::endl;
    }
    if (address && address != MAP_FAILED) {
      const int rc = trace("cleanup unmap", [&] { return ubsmem_shmem_unmap(address, bytes); });
      std::cerr << "cleanup unmap rc=" << rc << std::endl;
      if (!rc) address = nullptr;
    }
    if (created && !address) {
      const int rc = trace("cleanup deallocate", [&] { return ubsmem_shmem_deallocate(name.c_str()); });
      std::cerr << "cleanup deallocate rc=" << rc << std::endl;
      if (!rc) created = false;
    }
    if (initialized && !address)
      trace("cleanup finalize", [&] { return ubsmem_finalize(); });
    if (created) std::cerr << "OBJECT_MAY_REMAIN name=" << name << std::endl;
    return 1;
  }
}
