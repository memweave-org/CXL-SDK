#include "pcc/btree.h"
#include "bwtree.h"
#include "shm/mm.h"

#include <iostream>
#include <cstring>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/shm.h>
#include <errno.h>
#include <thread>
#include <chrono>

using namespace wangziqi2013::bwtree;

// Define KeyComparator and KeyEqualityChecker for BwTree
class BwTreeKeyComparator {
public:
    inline bool operator()(const int64_t k1, const int64_t k2) const {
        return k1 < k2;
    }
    BwTreeKeyComparator(int dummy = 0) { (void)dummy; }
};

class BwTreeKeyEqualityChecker {
public:
    inline bool operator()(const int64_t k1, const int64_t k2) const {
        return k1 == k2;
    }
    BwTreeKeyEqualityChecker(int dummy = 0) { (void)dummy; }
};

using TreeType = BwTree<int64_t, int64_t, BwTreeKeyComparator, BwTreeKeyEqualityChecker>;

namespace pcc {

btree::btree() : tree_ptr_(nullptr), ptr_storage_(nullptr), ptr_storage_size_(0), 
                 is_creator_(false), thread_num_(1) {
}

btree::~btree() {
    if (tree_ptr_ != nullptr) {
        TreeType* tree = static_cast<TreeType*>(tree_ptr_);
        // Only destroy if we're the creator
        if (is_creator_) {
            tree->UpdateThreadLocal(1);
            tree->~TreeType();
            cacheable.free(tree_ptr_);
            
            // Unlink POSIX shared memory object
            std::string posix_shm_name = shm_id_;
            for (size_t i = 0; i < posix_shm_name.length(); ++i) {
                if (posix_shm_name[i] == '/') {
                    posix_shm_name[i] = '_';
                }
            }
            shm_unlink(posix_shm_name.c_str());
        }
        tree_ptr_ = nullptr;
    }
    
    // Unmap pointer storage
    if (ptr_storage_ != nullptr) {
        munmap(ptr_storage_, ptr_storage_size_);
        ptr_storage_ = nullptr;
    }
}

btree* btree::init(const std::string& shm_id, bool is_creator, int thread_num) {
    btree* instance = new btree();
    instance->shm_id_ = shm_id;
    instance->is_creator_ = is_creator;
    instance->thread_num_ = thread_num;
    
    // Use shm_id as POSIX shared memory object name (must not contain '/')
    // Convert shm_id to a valid POSIX shm name
    std::string posix_shm_name = shm_id;
    // Replace '/' with '_' for POSIX shm_open compatibility
    for (size_t i = 0; i < posix_shm_name.length(); ++i) {
        if (posix_shm_name[i] == '/') {
            posix_shm_name[i] = '_';
        }
    }
    
    // Use a separate shared memory region to store the BwTree pointer
    // This ensures all processes can find the BwTree instance at the same address
    size_t ptr_size = sizeof(void*);
    int shm_fd = shm_open(posix_shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        std::cerr << "[BwTree::init] Failed to open/create POSIX shared memory: " 
                  << posix_shm_name << " (" << strerror(errno) << ")" << std::endl;
        delete instance;
        return nullptr;
    }
    
    // Set size for pointer storage
    if (is_creator) {
        if (ftruncate(shm_fd, ptr_size) < 0) {
            std::cerr << "[BwTree::init] Failed to truncate shared memory: " << strerror(errno) << std::endl;
            close(shm_fd);
            delete instance;
            return nullptr;
        }
    }
    
    // Map shared memory for pointer storage (use a different fixed address)
    // Offset from cacheable base to avoid conflict
    void* ptr_storage_addr = reinterpret_cast<void*>(0xcaffe0000000 + 0x1000000); // Offset by 16MB
    void* ptr_storage = mmap(ptr_storage_addr, ptr_size, PROT_READ | PROT_WRITE, 
                            MAP_SHARED | MAP_FIXED, shm_fd, 0);
    close(shm_fd);
    
    if (ptr_storage == MAP_FAILED || ptr_storage != ptr_storage_addr) {
        std::cerr << "[BwTree::init] Failed to mmap pointer storage: " << strerror(errno) << std::endl;
        if (is_creator) {
            shm_unlink(posix_shm_name.c_str());
        }
        delete instance;
        return nullptr;
    }
    
    void** tree_ptr_storage = static_cast<void**>(ptr_storage);
    
    if (is_creator) {
        // Create BwTree instance in cacheable allocator (shared memory)
        size_t tree_size = sizeof(TreeType);
        void* alloc_base = cacheable.malloc(tree_size);
        if (!alloc_base) {
            std::cerr << "[BwTree::init] Failed to allocate memory for BwTree" << std::endl;
            munmap(ptr_storage, ptr_size);
            shm_unlink(posix_shm_name.c_str());
            delete instance;
            return nullptr;
        }
        
        TreeType* tree = new (alloc_base) TreeType(true, BwTreeKeyComparator(1), BwTreeKeyEqualityChecker(1));
        tree->UpdateThreadLocal(thread_num);
        tree->AssignGCID(0);
        
        // Store pointer in shared memory so other processes can find it
        *tree_ptr_storage = alloc_base;
        
        instance->tree_ptr_ = alloc_base;
        std::cout << "[BwTree::init] Creator: Created BwTree at " << alloc_base 
                  << " (shm_id: " << shm_id << ")" << std::endl;
        
        // Debug: Print epoch manager current_epoch_p address and verify it's in shared memory
        void* epoch_addr = (void*)tree->epoch_manager.current_epoch_p;
        std::cout << "[BwTree::init] Creator: epoch_manager.current_epoch_p = " << epoch_addr << std::endl;
        
        // Check if epoch_p is in the shared memory range (approximate check)
        // The cacheable allocator base is 0xcaffe0000000, so addresses should be in that range
        if (reinterpret_cast<uintptr_t>(epoch_addr) < 0x7fff00000000ULL) {
            std::cerr << "[BwTree::init] WARNING: current_epoch_p appears to be in heap, not shared memory!" << std::endl;
            std::cerr << "[BwTree::init] This may cause multi-process access issues." << std::endl;
        }
    } else {
        // Attach to existing BwTree instance
        // Wait for creator to initialize the pointer
        int retries = 0;
        const int max_retries = 50;
        while (retries < max_retries && *tree_ptr_storage == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            retries++;
        }
        
        if (*tree_ptr_storage == nullptr) {
            std::cerr << "[BwTree::init] Attacher: Failed to find tree pointer after " 
                      << max_retries << " retries" << std::endl;
            munmap(ptr_storage, ptr_size);
            delete instance;
            return nullptr;
        }
        
        instance->tree_ptr_ = *tree_ptr_storage;
        TreeType* tree = static_cast<TreeType*>(instance->tree_ptr_);
        std::cout << "[BwTree::init] Attacher: Attached to BwTree at " << instance->tree_ptr_ 
                  << " (shm_id: " << shm_id << ")" << std::endl;
        
        // Debug: Print epoch manager current_epoch_p address
        std::cout << "[BwTree::init] Attacher: epoch_manager.current_epoch_p = " 
                  << (void*)tree->epoch_manager.current_epoch_p << std::endl;
        
        // Verify current_epoch_p is accessible
        if (tree->epoch_manager.current_epoch_p != nullptr) {
            int test_count = tree->epoch_manager.current_epoch_p->active_thread_count.load();
            std::cout << "[BwTree::init] Attacher: current_epoch_p->active_thread_count = " 
                      << test_count << std::endl;
        } else {
            std::cerr << "[BwTree::init] Attacher: ERROR: current_epoch_p is nullptr!" << std::endl;
        }
    }
    
    // Store ptr_storage info for cleanup
    instance->ptr_storage_ = ptr_storage;
    instance->ptr_storage_size_ = ptr_size;
    
    return instance;
}

void btree::cleanup(const std::string& shm_id) {
    // Convert shm_id to POSIX shared memory name
    std::string posix_shm_name = shm_id;
    for (size_t i = 0; i < posix_shm_name.length(); ++i) {
        if (posix_shm_name[i] == '/') {
            posix_shm_name[i] = '_';
        }
    }
    shm_unlink(posix_shm_name.c_str());
}

bool btree::insert(const key_type& key, const mapped_type& value) {
    TreeType* tree = static_cast<TreeType*>(get_tree());
    if (!tree) return false;
    
    std::vector<mapped_type> old_values;
    tree->GetValue(key, old_values);
    
    if (old_values.empty()) {
        // Insert new key-value pair
        bool result = tree->Insert(key, value);
        return result;
    } else {
        // Update existing: Delete old and insert new
        // BwTree doesn't have Update, so we delete old value and insert new
        tree->Delete(key, old_values[0]);
        tree->Insert(key, value);
        return false;
    }
}

bool btree::update(const key_type& key, const mapped_type& value) {
    TreeType* tree = static_cast<TreeType*>(get_tree());
    if (!tree) return false;
    
    std::vector<mapped_type> old_values;
    tree->GetValue(key, old_values);
    
    if (old_values.empty()) {
        return false;
    }
    
    // BwTree doesn't have Update, so we delete old value and insert new
    tree->Delete(key, old_values[0]);
    tree->Insert(key, value);
    return true;
}

bool btree::find(const key_type& key, mapped_type& value) const {
    TreeType* tree = static_cast<TreeType*>(get_tree());
    if (!tree) {
        std::cerr << "[btree::find] Tree pointer is null!" << std::endl;
        return false;
    }
    
    try {
        std::cout << "[btree::find] Calling GetValue for key " << key << " (process " << getpid() << ")" << std::endl;
        std::cout.flush();
        
        std::vector<mapped_type> values;
        tree->GetValue(key, values);
        
        std::cout << "[btree::find] GetValue returned " << values.size() << " values" << std::endl;
        std::cout.flush();
        
        if (values.empty()) {
            return false;
        }
        
        value = values[0];
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[btree::find] Exception: " << e.what() << std::endl;
        return false;
    } catch (...) {
        std::cerr << "[btree::find] Unknown exception" << std::endl;
        return false;
    }
}

bool btree::erase(const key_type& key) {
    TreeType* tree = static_cast<TreeType*>(get_tree());
    if (!tree) return false;
    
    std::vector<mapped_type> old_values;
    tree->GetValue(key, old_values);
    
    if (old_values.empty()) {
        return false;
    }
    
    // BwTree Delete requires both key and value
    tree->Delete(key, old_values[0]);
    return true;
}

bool btree::contains(const key_type& key) const {
    TreeType* tree = static_cast<TreeType*>(get_tree());
    if (!tree) return false;
    
    std::vector<mapped_type> values;
    tree->GetValue(key, values);
    return !values.empty();
}

btree::size_type btree::size() const {
    // BwTree doesn't provide a direct size() method
    // This is a placeholder - you may need to track size separately
    return 0;
}

bool btree::empty() const {
    // BwTree doesn't provide a direct empty() method
    // This is a placeholder
    return false;
}

void btree::thread_init(int thread_id) {
    TreeType* tree = static_cast<TreeType*>(get_tree());
    if (!tree) {
        std::cerr << "[btree::thread_init] Tree pointer is null!" << std::endl;
        return;
    }
    
    // For multi-process, we need to ensure thread local storage is properly initialized
    // Only update thread local if we're the creator, or if it hasn't been initialized yet
    // For attacher, we should not call UpdateThreadLocal as it may destroy existing state
    if (is_creator_) {
        tree->UpdateThreadLocal(thread_num_);
    }
    tree->AssignGCID(thread_id);
    
    std::cout << "[btree::thread_init] Thread " << thread_id << " initialized for process " << getpid() << std::endl;
}

void btree::thread_cleanup(int thread_id) {
    TreeType* tree = static_cast<TreeType*>(get_tree());
    if (tree) {
        tree->UnregisterThread(thread_id);
    }
}

void* btree::get_tree() const {
    return tree_ptr_;
}

} // namespace pcc

