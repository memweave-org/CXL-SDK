#include "shm/ub.h"
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) try {
  if (argc == 3 && std::string(argv[1]) == "destroy") {
    destroy_ub_memory(argv[2]);
  } else if (argc == 6 && std::string(argv[1]) == "create") {
    const auto size = std::stoull(argv[3]);
    const auto numa = std::stoull(argv[5]);
    if (!size || size > SIZE_MAX / (1ul << 20) || numa > UINT32_MAX)
      throw std::runtime_error("Invalid size or NUMA id");
    create_ub_memory(argv[2], size << 20, argv[4], uint32_t(numa));
  } else {
    throw std::runtime_error("usage: ub_admin create NAME SIZE_MB PROVIDER_HOST NUMA | destroy NAME");
  }
  return 0;
} catch (const std::exception& e) {
  std::cerr << "ERROR " << e.what() << '\n';
  return 1;
}
