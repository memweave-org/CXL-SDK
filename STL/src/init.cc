#include "pcc/init.h"
#include "shm/mm.h"
#include "utils/sim_id.h"

#include <cctype>
#include <cstring>
#include <iostream>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdexcept>
#include <errno.h>

namespace pcc {

bool init_shm(const ShmConfig& config) {
    try {
        // Initialize thread info
        SimThreadInfo::worker_machine_count = 1;
        SimThreadInfo::worker_machine_id = 0;
        
        // Check if this is a DAX device (starts with /dev/dax)
        bool is_dax_device = (config.shm_path.find("/dev/dax") == 0);
        std::string actual_path = config.shm_path;
        size_t actual_size = config.shm_size;
        
        if (is_dax_device) {
            // For DAX devices, open directly (no create/truncate needed)
            int fd = open(actual_path.c_str(), O_RDWR);
            if (fd < 0) {
                std::cerr << "Failed to open DAX device: " << actual_path 
                          << " (" << strerror(errno) << ")" << std::endl;
                std::cerr << "Note: DAX devices require proper setup. Trying fallback to regular file..." << std::endl;
                
                // Fallback: try using /dev/shm as regular file
                actual_path = "/dev/shm/cxl_demo";
                is_dax_device = false;
                
                int fallback_fd = open(actual_path.c_str(), O_CREAT | O_RDWR, 0666);
                if (fallback_fd < 0) {
                    std::cerr << "Fallback also failed: " << strerror(errno) << std::endl;
                    return false;
                }
                
                // Set size for fallback file
                if (ftruncate(fallback_fd, actual_size) < 0) {
                    std::cerr << "Failed to truncate fallback file: " << strerror(errno) << std::endl;
                    close(fallback_fd);
                    return false;
                }
                close(fallback_fd);
            } else {
                // Successfully opened DAX device
                // Get device size
                off_t device_size = lseek(fd, 0, SEEK_END);
                lseek(fd, 0, SEEK_SET);
                
                if (device_size > 0 && (size_t)device_size < config.shm_size) {
                    std::cerr << "Warning: DAX device size (" << device_size 
                              << ") is smaller than requested (" << config.shm_size 
                              << "). Using device size." << std::endl;
                    actual_size = device_size;
                }
                
                // DAX devices require 2MB alignment
                const size_t dax_align = 2 * 1024 * 1024; // 2MB
                if (actual_size < dax_align) {
                    actual_size = dax_align;
                } else {
                    actual_size = (actual_size + dax_align - 1) & ~(dax_align - 1);
                }
                
                close(fd);
            }
        }
        
        if (!is_dax_device) {
            // For regular files, create if needed
            int fd = open(actual_path.c_str(), O_CREAT | O_RDWR, 0666);
            if (fd < 0) {
                std::cerr << "Failed to create/open shared memory file: " << actual_path 
                          << " (" << strerror(errno) << ")" << std::endl;
                return false;
            }
            
            // Set size
            struct stat st;
            if (fstat(fd, &st) == 0 && st.st_size < (off_t)actual_size) {
                if (ftruncate(fd, actual_size) < 0) {
                    std::cerr << "Failed to truncate shared memory file: " << strerror(errno) << std::endl;
                    close(fd);
                    return false;
                }
            }
            close(fd);
        }
        
        void *base_addr = reinterpret_cast<void *>(0xcaffe0000000);
        CacheableInitParams p{};
        p.device_path = actual_path.c_str();
        p.mmap_base = base_addr;
        p.size_bytes = actual_size;
        p.worker_machine_count = 1;
        p.worker_machine_id = 0;
        p.mem_type_cxl = is_dax_device;
        p.cxlalloc_heap_numa = config.cxlalloc_heap_numa;
        p.thread_count = config.cxlalloc_thread_count;
        if (p.thread_count == 0) {
          p.thread_count = 64;
        }
        std::string ab = config.allocator_backend;
        for (auto &ch : ab) {
          ch = static_cast<char>(
              std::tolower(static_cast<unsigned char>(ch)));
        }
        if (ab == "cxlalloc") {
          p.backend = CacheableAllocatorBackend::Cxlalloc;
        } else {
          p.backend = CacheableAllocatorBackend::Memkind;
        }
        init_cacheable_allocator_unified(p);
        
        if (is_dax_device) {
            std::cout << "DAX device initialized: " << actual_path 
                      << " (size: " << actual_size << " bytes)" << std::endl;
        } else {
            std::cout << "Shared memory initialized: " << actual_path 
                      << " (size: " << actual_size << " bytes)" << std::endl;
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize shared memory: " << e.what() << std::endl;
        return false;
    }
}

void cleanup_shm() {
    // Cleanup is handled by destructors
    std::cout << "Shared memory cleanup completed" << std::endl;
}

std::string get_shm_id(const std::string& base_name, uint64_t instance_id) {
    return "/dev/shm/" + base_name + "_" + std::to_string(instance_id);
}

} // namespace pcc
