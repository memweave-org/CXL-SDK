#include "internal/lsmmalloc_impl.h"
#include "utils/log.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace lsmallocimpl;

thread_local cid_t tid;
std::unique_ptr<LockFreeLogAllocator> global_allocator = nullptr;
void* shm_base = nullptr;
constexpr size_t cxl_data_size = 4ULL * 1024 * 1024 * 1024;
constexpr const char* cxl_data_path = "/dev/shm/logcxl_data";
constexpr size_t cxl_hashmap_size = 1ULL * 1024 * 1024 * 1024;
constexpr const char* cxl_hashmap_path = "/dev/shm/logcxl_hashmap";
constexpr size_t max_objects = 1000000;
constexpr int thread_num = 32;
constexpr int num_buckets = 1024;

const size_t MAX_TEST_OBJECTS = 100;

template <typename T>
void perform_operation(lsmallocimpl::cid_t thread_id) {
    tid = thread_id;
    int gc_thread_id = global_allocator->register_thread();

    thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> object_id_dist(1, MAX_TEST_OBJECTS - 1);
    std::uniform_int_distribution<> data_dist(0, 100);

    int object_id = object_id_dist(gen);

    global_allocator->acquire<int>(object_id, tid);

    // Perform some random reads and writes
    for (int i = 0; i < 50000; ++i) {
        int data = data_dist(gen);
        global_allocator->write<T>(object_id, data, tid);

        T* read_val = global_allocator->read<T>(object_id, tid);
        T val = *read_val;
        (void)val;
    }

    global_allocator->release<int>(object_id, tid);
    global_allocator->unregister_thread(gc_thread_id);
}

TEST(RandomOpsTest, IntTest) {
    using T = int;

    lsmalloc_config config = {cxl_data_path, cxl_data_size, cxl_hashmap_path, cxl_hashmap_size,
                              max_objects,   true,          thread_num,       num_buckets};
    global_allocator = init_allocator(config);
    for (size_t i = 0; i < MAX_TEST_OBJECTS; ++i) {
        global_allocator->allocate<T>(0);
    }
    size_t thread_num = 16;
    size_t iter_num = 10;
    LOG_INFO("Starting random operations test with {} threads...", thread_num);
    std::vector<std::thread> threads;
    for (size_t iter = 0; iter < iter_num; ++iter) {
        LOG_INFO("Starting iteration {}", iter + 1);
        for (lsmallocimpl::cid_t i = 0; i < thread_num; ++i) {
            threads.emplace_back(perform_operation<T>, i);
        }
        for (auto& t : threads) {
            t.join();
        }
        threads.clear();
    }
    LOG_INFO("All iterations finished. Waiting for GC to clean up...");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (size_t i = 0; i < MAX_TEST_OBJECTS; ++i) {
        global_allocator->free<T>(i, 0);
    }
    LOG_INFO("Test finished.");
    global_allocator->display_status();
}