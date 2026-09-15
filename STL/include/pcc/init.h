#pragma once

#include <cstdint>
#include <string>

namespace pcc {

// Shared memory initialization configuration
struct ShmConfig {
    std::string shm_path;      // Path to shared memory (e.g., "/dev/shm/cxl")
    std::string shm_id;        // Unique identifier for this data structure instance
    size_t shm_size;           // Size of shared memory in bytes
    int thread_num;            // Number of threads
    bool is_creator;           // Whether this process creates the shared memory
    /** memkind (default) or cxlalloc (requires shm-lib WITH_CXLALLOC) */
    std::string allocator_backend = "memkind";
    uint16_t cxlalloc_thread_count = 64;
    int8_t cxlalloc_heap_numa = -1;
};

// Initialize shared memory allocator
// This must be called before creating any pcc::btree or pcc::clevelhash instances
// @param config: Shared memory configuration
// @return true if successful, false otherwise
bool init_shm(const ShmConfig& config);

// Cleanup shared memory allocator
// Call this when done with all data structures
void cleanup_shm();

// Get a unique shared memory ID for a data structure
// @param base_name: Base name (e.g., "bwtree", "clevelhash")
// @param instance_id: Instance identifier (e.g., 123)
// @return Full shared memory path
std::string get_shm_id(const std::string& base_name, uint64_t instance_id);

} // namespace pcc

