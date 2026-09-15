#include "pcc/btree.h"
#include "pcc/clevelhash.h"
#include "pcc/init.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <vector>

void demo_btree() {
    std::cout << "\n=== BwTree Demo ===" << std::endl;
    
    // Initialize shared memory with a file-backed mapping
    pcc::ShmConfig config;
    config.shm_path = "/tmp/cxl-sdk-demo-btree";
    config.shm_id = "demo_btree";
    config.shm_size = 256ULL * 1024 * 1024; // 256 MiB
    config.thread_num = 2;
    config.is_creator = true;
    
    if (!pcc::init_shm(config)) {
        std::cerr << "Failed to initialize shared memory" << std::endl;
        return;
    }
    
    // Create or attach to BwTree
    std::string tree_id = pcc::get_shm_id("bwtree", 123);
    pcc::btree* tree = pcc::btree::init(tree_id, true, 2);
    
    if (!tree) {
        std::cerr << "Failed to create BwTree" << std::endl;
        pcc::cleanup_shm();
        return;
    }
    
    // Initialize threads
    tree->thread_init(0);
    
    // Insert some key-value pairs
    std::cout << "Inserting key-value pairs..." << std::endl;
    for (int64_t i = 1; i <= 10; ++i) {
        bool inserted = tree->insert(i, i * 100);
        std::cout << "  Inserted (" << i << ", " << (i * 100) << "): " 
                  << (inserted ? "new" : "updated") << std::endl;
    }
    
    // Find values
    std::cout << "\nFinding values..." << std::endl;
    for (int64_t i = 1; i <= 10; ++i) {
        int64_t value;
        if (tree->find(i, value)) {
            std::cout << "  Key " << i << " -> Value " << value << std::endl;
        } else {
            std::cout << "  Key " << i << " not found" << std::endl;
        }
    }
    
    // Update a value
    std::cout << "\nUpdating key 5..." << std::endl;
    tree->update(5, 999);
    int64_t value;
    if (tree->find(5, value)) {
        std::cout << "  Key 5 -> Value " << value << std::endl;
    }
    
    // Erase a key
    std::cout << "\nErasing key 3..." << std::endl;
    tree->erase(3);
    if (!tree->contains(3)) {
        std::cout << "  Key 3 successfully erased" << std::endl;
    }
    
    // Cleanup
    delete tree;
    pcc::cleanup_shm();
}

void demo_clevelhash() {
    std::cout << "\n=== ClevelHash Demo ===" << std::endl;
    
    // Initialize shared memory with a file-backed mapping
    pcc::ShmConfig config;
    config.shm_path = "/tmp/cxl-sdk-demo-clevelhash";
    config.shm_id = "demo_clevelhash";
    config.shm_size = 256ULL * 1024 * 1024; // 256 MiB
    config.thread_num = 2;
    config.is_creator = true;
    
    if (!pcc::init_shm(config)) {
        std::cerr << "Failed to initialize shared memory" << std::endl;
        return;
    }
    
    // Create ClevelHash
    std::string hash_id = pcc::get_shm_id("clevelhash", 456);
    pcc::clevelhash* hash = pcc::clevelhash::init(hash_id, true, 2);
    
    if (!hash) {
        std::cerr << "Failed to create ClevelHash" << std::endl;
        pcc::cleanup_shm();
        return;
    }
    
    // Insert some key-value pairs
    std::cout << "Inserting key-value pairs..." << std::endl;
    for (uint64_t i = 1; i <= 10; ++i) {
        bool inserted = hash->insert(i, i * 200);
        std::cout << "  Inserted (" << i << ", " << (i * 200) << "): " 
                  << (inserted ? "new" : "updated") << std::endl;
    }
    
    // Find values
    std::cout << "\nFinding values..." << std::endl;
    for (uint64_t i = 1; i <= 10; ++i) {
        uint64_t value;
        if (hash->find(i, value)) {
            std::cout << "  Key " << i << " -> Value " << value << std::endl;
        } else {
            std::cout << "  Key " << i << " not found" << std::endl;
        }
    }
    
    // Cleanup
    delete hash;
    pcc::cleanup_shm();
}

int main(int argc, char* argv[]) {
    std::cout << "Shared-Memory STL Example" << std::endl;
    std::cout << "========================" << std::endl;
    
    if (argc > 1 && std::string(argv[1]) == "clevelhash") {
        demo_clevelhash();
    } else {
        demo_btree();
    }
    
    return 0;
}
