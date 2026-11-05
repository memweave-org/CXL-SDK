// Independent UBSM cache visibility probe. Control traffic never uses UB memory.
#include <ubs_mem.h>
#include <ubs_mem_def.h>
#include <sys/mman.h>
#include <sched.h>
#include <unistd.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

constexpr uint64_t OLD = 0x1122334455667788ULL;
constexpr uint64_t NEW = 0x8877665544332211ULL;
static_assert(__atomic_always_lock_free(sizeof(uint64_t), nullptr),
              "NC probe requires native lock-free 64-bit atomics");
static void check(const char* op, int rc) {
  std::cerr << op << " rc=" << rc << std::endl;
  if (rc) throw std::runtime_error(op);
}
static void barrier() {
#if defined(__aarch64__)
  asm volatile("dsb sy" ::: "memory");
#elif defined(__x86_64__)
  asm volatile("mfence" ::: "memory");
#else
#error Unsupported architecture
#endif
}
static void flush(void* p) {
  barrier();
#if defined(__aarch64__)
  asm volatile("dc cvac, %0" :: "r"(p) : "memory");
#else
  asm volatile("clflush (%0)" :: "r"(p) : "memory");
#endif
  barrier();
}
static void invalidate(void* p) {
  barrier();
#if defined(__aarch64__)
  // EL0-compatible clean+invalidate; A has never dirtied its read-only line.
  asm volatile("dc civac, %0" :: "r"(p) : "memory");
#else
  asm volatile("clflush (%0)" :: "r"(p) : "memory");
#endif
  barrier();
}
static uint64_t number(const char* s) {
  size_t end; std::string str(s);
  if (str.empty() || str[0] == '-') throw std::runtime_error("invalid number");
  auto value = std::stoull(str, &end, 0);
  if (end != str.size()) throw std::runtime_error("invalid number");
  return value;
}
int main(int argc, char** argv) {
  bool initialized=false, created=false, owned=false;
  void* address=nullptr;
  size_t bytes=0;
  std::string name;
  int result=1;
  try {
    if (argc != 8) throw std::runtime_error("usage: worker A|B NAME PROVIDER SIZE_MB BASE OFFSET CPU");
    const std::string role=argv[1];
    if (role!="A" && role!="B") throw std::runtime_error("invalid role");
    name=argv[2];
    ubs_mem_provider_t provider{};
    if (!argv[3][0] || std::strlen(argv[3])>=sizeof(provider.host_name)) throw std::runtime_error("invalid provider");
    std::strcpy(provider.host_name,argv[3]);
    provider.socket_id=provider.numa_id=provider.port_id=UINT32_MAX;
    const auto mb=number(argv[4]), base=number(argv[5]), offset=number(argv[6]), cpu=number(argv[7]);
    if (mb<4 || mb%4 || mb>SIZE_MAX/(1UL<<20)) throw std::runtime_error("size must be a multiple of 4 MiB");
    bytes=mb<<20;
    if (!base || base% (4UL<<20) || base>UINTPTR_MAX-bytes || offset%8 || offset>bytes-8 || cpu>=CPU_SETSIZE)
      throw std::runtime_error("invalid address, offset or CPU");
    cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(cpu,&cpus);
    if (sched_setaffinity(0,sizeof(cpus),&cpus)) throw std::runtime_error("CPU affinity failed");
    const bool nc = std::getenv("UB_MICRO_NC") != nullptr;
    const std::string map_mode = std::getenv("NC_MAP_MODE") ? std::getenv("NC_MAP_MODE") : "noreplace";
    if (map_mode!="shared" && map_mode!="noreplace") throw std::runtime_error("invalid NC_MAP_MODE");
    const int map_flags = MAP_SHARED | ((nc && map_mode=="shared") ? 0 : MAP_FIXED_NOREPLACE);
    const uint64_t allocation_flags = nc ? UBSM_FLAG_NONCACHE | UBSM_FLAG_WR_DELAY_COMP : UBSM_FLAG_CACHE;
    std::cerr << "CONFIG role=" << role << " mode=" << (nc ? "NC" : "CACHE")
              << " bytes=" << bytes << " allocation_flags=" << allocation_flags
              << " map_flags=" << map_flags << " base=" << reinterpret_cast<void*>(base) << std::endl;
    ubsmem_options_t options{};
    std::cerr << "BEGIN attributes" << std::endl;
    check("attributes",ubsmem_init_attributes(&options));
    std::cerr << "BEGIN initialize" << std::endl;
    check("initialize",ubsmem_initialize(&options)); initialized=true;
    if (role=="B") {
      std::cerr << "BEGIN allocate" << std::endl;
      check("allocate",ubsmem_shmem_allocate_with_provider(&provider,name.c_str(),bytes,0660,allocation_flags));
      created=true;
    }
    int prot=nc || role=="B" ? PROT_READ|PROT_WRITE : PROT_READ;
    std::cerr << "BEGIN map name=" << name << " prot=" << prot << std::endl;
    check("map",ubsmem_shmem_map(reinterpret_cast<void*>(base),bytes,prot,map_flags,name.c_str(),0,&address));
    if (address!=reinterpret_cast<void*>(base)) throw std::runtime_error("SDK did not honor exact VA");
    if (!nc) {
      std::cerr << "BEGIN ownership" << std::endl;
      check("ownership",ubsmem_shmem_set_ownership(name.c_str(),address,bytes,prot)); owned=true;
    }
    void* target=static_cast<char*>(address)+offset;
    auto* word=static_cast<volatile uint64_t*>(target);
    std::cerr << "role=" << role << " name=" << name << " base=" << address << " target=" << target
              << " bytes=" << bytes << " prot=" << prot << " cpu=" << cpu << std::endl;
    if (role=="B") {
      std::cerr << "BEGIN initial write" << std::endl;
      *word=OLD; if (nc) barrier(); else flush(target);
    }
    std::cout << "UBMICRO READY" << std::endl;
    std::string command;
    while (std::getline(std::cin,command)) {
      std::cerr << "BEGIN command " << command << std::endl;
      if (nc && command=="read") {
        barrier(); auto value=*word; barrier();
        std::cout << "UBMICRO VALUE " << value << std::endl;
      } else if (nc && command=="write") {
        *word=role=="B" ? NEW : OLD; barrier();
        std::cout << "UBMICRO OK" << std::endl;
      } else if (nc && command.rfind("cas ",0)==0) {
        std::istringstream input(command);
        std::string op, expected_text, desired_text, extra;
        if (!(input >> op >> expected_text >> desired_text) || (input >> extra))
          throw std::runtime_error("usage: cas EXPECTED DESIRED");
        uint64_t expected=number(expected_text.c_str());
        const uint64_t desired=number(desired_text.c_str());
        barrier();
        const bool success=__atomic_compare_exchange_n(word,&expected,desired,false,
                                                       __ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
        barrier();
        // On failure expected must contain the actual old value; on success it is unchanged.
        std::cout << "UBMICRO CAS " << success << ' ' << expected << std::endl;
      } else if (nc && command.rfind("fetch_add ",0)==0) {
        std::istringstream input(command);
        std::string op, amount_text, extra;
        if (!(input >> op >> amount_text) || (input >> extra))
          throw std::runtime_error("usage: fetch_add AMOUNT");
        const uint64_t amount=number(amount_text.c_str());
        barrier();
        const uint64_t previous=__atomic_fetch_add(word,amount,__ATOMIC_SEQ_CST);
        barrier();
        std::cout << "UBMICRO FETCH_ADD " << previous << std::endl;
      } else if (!nc && command=="prime" && role=="A") {
        invalidate(target); auto value=*word; barrier();
        std::cout << "UBMICRO VALUE " << value << std::endl;
      } else if (command=="read" && role=="A") {
        auto value=*word; barrier();
        std::cout << "UBMICRO VALUE " << value << std::endl;
      } else if (command=="write" && role=="B") {
        *word=NEW; barrier(); std::cout << "UBMICRO OK" << std::endl;
      } else if (!nc && command=="flush" && role=="B") {
        flush(target); std::cout << "UBMICRO OK" << std::endl;
      } else if (!nc && command=="invalidate" && role=="A") {
        invalidate(target); auto value=*word; barrier();
        std::cout << "UBMICRO VALUE " << value << std::endl;
      } else if (command=="close" || (command=="release" && role=="B")) {
        if (owned) {
          std::cerr << "BEGIN ownership NONE" << std::endl;
          check("ownership NONE",ubsmem_shmem_set_ownership(name.c_str(),address,bytes,PROT_NONE)); owned=false;
        }
        std::cerr << "BEGIN unmap" << std::endl;
        check("unmap",ubsmem_shmem_unmap(address,bytes)); address=nullptr;
        // Coordinator sends release only after A has finalized and exited successfully.
        if (command=="release") {
          std::cerr << "BEGIN deallocate" << std::endl;
          check("deallocate",ubsmem_shmem_deallocate(name.c_str())); created=false;
        }
        std::cerr << "BEGIN finalize" << std::endl;
        check("finalize",ubsmem_finalize()); initialized=false;
        std::cout << "UBMICRO CLOSED" << std::endl;
        result=0; break;
      } else throw std::runtime_error("invalid command for role");
    }
  } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << std::endl; }
  // On failure never free the object: the remote mapping may still be alive.
  if (owned && address) {
    int rc=ubsmem_shmem_set_ownership(name.c_str(),address,bytes,PROT_NONE);
    std::cerr << "cleanup ownership rc=" << rc << std::endl;
  }
  if (address && address!=MAP_FAILED) {
    int rc=ubsmem_shmem_unmap(address,bytes);
    std::cerr << "cleanup unmap rc=" << rc << std::endl;
    if (!rc) address=nullptr;
  }
  if (initialized && !address) std::cerr << "cleanup finalize rc=" << ubsmem_finalize() << std::endl;
  if (created) std::cerr << "OBJECT_MAY_REMAIN name=" << name << std::endl;
  return result;
}
