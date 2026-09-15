#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

// Forward declarations - will include full header in implementation

namespace pcc {

// Key comparator for BwTree
class KeyComparator {
public:
    inline bool operator()(const int64_t k1, const int64_t k2) const {
        return k1 < k2;
    }
    KeyComparator(int dummy = 0) { (void)dummy; }
};

// Key equality checker for BwTree
class KeyEqualityChecker {
public:
    inline bool operator()(const int64_t k1, const int64_t k2) const {
        return k1 == k2;
    }
    KeyEqualityChecker(int dummy = 0) { (void)dummy; }
};

// BwTree wrapper with std::map-like interface
class btree {
public:
    using key_type = int64_t;
    using mapped_type = int64_t;
    using value_type = std::pair<const key_type, mapped_type>;
    using size_type = size_t;

    // Constructor - creates or attaches to shared BwTree
    btree();
    
    // Destructor
    ~btree();

    // Initialize shared memory for BwTree
    // @param shm_id: Shared memory identifier (e.g., "/dev/shm/bwtree_123")
    // @param is_creator: true if this process creates the tree, false if attaching
    // @param thread_num: Number of threads that will access this tree
    static btree* init(const std::string& shm_id, bool is_creator = true, int thread_num = 1);

    // Cleanup shared memory (only call from creator process)
    static void cleanup(const std::string& shm_id);

    // Insert or update a key-value pair
    // Returns true if inserted, false if updated
    bool insert(const key_type& key, const mapped_type& value);
    
    // Update existing key-value pair
    // Returns true if key exists and was updated, false otherwise
    bool update(const key_type& key, const mapped_type& value);
    
    // Find value by key
    // Returns true if found, false otherwise
    bool find(const key_type& key, mapped_type& value) const;
    
    // Erase key-value pair
    // Returns true if key existed and was erased, false otherwise
    bool erase(const key_type& key);
    
    // Check if key exists
    bool contains(const key_type& key) const;
    
    // Get size (approximate, may not be exact in concurrent scenarios)
    size_type size() const;
    
    // Check if empty
    bool empty() const;
    
    // Thread initialization (call from each thread that will use this tree)
    void thread_init(int thread_id);
    
    // Thread cleanup (call when thread is done)
    void thread_cleanup(int thread_id);

private:
    void* tree_ptr_;  // Pointer to BwTree instance in shared memory
    void* ptr_storage_; // Pointer to shared memory region storing tree_ptr_
    size_t ptr_storage_size_; // Size of ptr_storage_ region
    bool is_creator_; // Whether this process created the shared memory
    std::string shm_id_;
    int thread_num_;
    
    // Helper to get the actual BwTree pointer
    void* get_tree() const;
};

} // namespace pcc

