#pragma once

#include <cstdint>
#include <string>
#include <memory>

namespace pcc {

// ClevelHash wrapper with std::unordered_map-like interface
class clevelhash {
public:
    using key_type = uint64_t;
    using mapped_type = uint64_t;
    using value_type = std::pair<const key_type, mapped_type>;
    using size_type = size_t;

    // Constructor
    clevelhash();
    
    // Destructor
    ~clevelhash();

    // Initialize shared memory for ClevelHash
    // @param shm_id: Shared memory identifier (e.g., "/dev/shm/clevelhash_123")
    // @param is_creator: true if this process creates the hash table, false if attaching
    // @param thread_num: Number of threads that will access this hash table
    static clevelhash* init(const std::string& shm_id, bool is_creator = true, int thread_num = 1);

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
    
    // Get size (approximate)
    size_type size() const;
    
    // Check if empty
    bool empty() const;
    
    // Thread initialization (call from each thread that will use this hash table)
    void thread_init(int thread_id);
    
    // Thread cleanup (call when thread is done)
    void thread_cleanup(int thread_id);

private:
    void* hash_ptr_;  // Pointer to ClevelHash instance in shared memory
    bool is_creator_; // Whether this process created the shared memory
    std::string shm_id_;
    int thread_num_;
    
    // Helper to get the actual ClevelHash pointer
    void* get_hash() const;
};

} // namespace pcc

