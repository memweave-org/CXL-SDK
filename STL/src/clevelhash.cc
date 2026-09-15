#include "pcc/clevelhash.h"
#include "shm/mm.h"
#include "clevel_hash.hpp"

#include <iostream>
#include <cstring>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/shm.h>
#include <errno.h>

using clevel_hash_type = clevel_hash<uint64_t, uint64_t>;

namespace pcc {

clevelhash::clevelhash() : hash_ptr_(nullptr), is_creator_(false), thread_num_(1) {
}

clevelhash::~clevelhash() {
    if (hash_ptr_ != nullptr && is_creator_) {
        clevel_hash_type* hash = static_cast<clevel_hash_type*>(hash_ptr_);
        delete hash;
        hash_ptr_ = nullptr;
    }
}

clevelhash* clevelhash::init(const std::string& shm_id, bool is_creator, int thread_num) {
    clevelhash* instance = new clevelhash();
    instance->shm_id_ = shm_id;
    instance->is_creator_ = is_creator;
    instance->thread_num_ = thread_num;
    
    if (is_creator) {
        // Create new ClevelHash instance
        clevel_hash_type* hash = new clevel_hash_type();
        hash->set_thread_num(thread_num);
        instance->hash_ptr_ = hash;
    } else {
        // For now, we'll create a new instance and attach
        // In a real implementation, you'd need to use PMDK or similar
        // to create persistent shared memory
        clevel_hash_type* hash = new clevel_hash_type();
        hash->set_thread_num(thread_num);
        instance->hash_ptr_ = hash;
        instance->is_creator_ = false; // Mark as not creator since we're attaching
    }
    
    return instance;
}

void clevelhash::cleanup(const std::string& shm_id) {
    // Cleanup would depend on the persistence mechanism used
    shm_unlink(shm_id.c_str());
}

bool clevelhash::insert(const key_type& key, const mapped_type& value) {
    clevel_hash_type* hash = static_cast<clevel_hash_type*>(get_hash());
    if (!hash) return false;
    
    auto result = hash->search(key);
    if (result.found) {
        hash->update(std::make_pair(key, value), 0);
        return false; // Updated
    } else {
        hash->insert(std::make_pair(key, value), 0, 0);
        return true; // Inserted
    }
}

bool clevelhash::update(const key_type& key, const mapped_type& value) {
    clevel_hash_type* hash = static_cast<clevel_hash_type*>(get_hash());
    if (!hash) return false;
    
    auto result = hash->search(key);
    if (result.found) {
        hash->update(std::make_pair(key, value), 0);
        return true;
    }
    return false;
}

bool clevelhash::find(const key_type& key, mapped_type& value) const {
    clevel_hash_type* hash = static_cast<clevel_hash_type*>(get_hash());
    if (!hash) return false;
    
    auto result = hash->search(key);
    if (result.found && result.value) {
        value = result.value->second;
        return true;
    }
    return false;
}

bool clevelhash::erase(const key_type& key) {
    clevel_hash_type* hash = static_cast<clevel_hash_type*>(get_hash());
    if (!hash) return false;
    
    auto result = hash->search(key);
    if (result.found) {
        hash->erase(key, 0);  // erase takes (key, thread_id)
        return true;
    }
    return false;
}

bool clevelhash::contains(const key_type& key) const {
    clevel_hash_type* hash = static_cast<clevel_hash_type*>(get_hash());
    if (!hash) return false;
    
    auto result = hash->search(key);
    return result.found;
}

clevelhash::size_type clevelhash::size() const {
    // ClevelHash doesn't provide a direct size() method
    return 0;
}

bool clevelhash::empty() const {
    return false;
}

void clevelhash::thread_init(int thread_id) {
    // ClevelHash handles thread initialization internally
    (void)thread_id;
}

void clevelhash::thread_cleanup(int thread_id) {
    // ClevelHash handles thread cleanup internally
    (void)thread_id;
}

void* clevelhash::get_hash() const {
    return hash_ptr_;
}

} // namespace pcc

