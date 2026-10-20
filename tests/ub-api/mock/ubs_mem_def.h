#pragma once
// Test-only ABI fixture based on the supplied API descriptions. Not vendor headers.
#include <cstdint>
constexpr uint64_t UBSM_FLAG_ONLY_IMPORT_NONCACHE = 1;
constexpr uint64_t UBSM_FLAG_WR_DELAY_COMP = 2;
struct ubsmem_options_t {};
struct ubs_mem_provider_t {
  char host_name[64];
  uint32_t socket_id, numa_id, port_id;
};
