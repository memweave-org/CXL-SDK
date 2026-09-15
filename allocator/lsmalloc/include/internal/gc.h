#pragma once

#include <atomic>
#include <functional>
#include <list>
#include <thread>

constexpr size_t MAX_CID = 1024;
constexpr size_t GC_CHECK_PERIOD_MS = 100;
using CleanupCallback = std::function<void(void*, size_t)>;

struct RetiredNode {
    void* addr;
    size_t size;
    int64_t retire_epoch;
    CleanupCallback callback;

    RetiredNode(void* addr, size_t size, int64_t retire_epoch, CleanupCallback callback)
        : addr(addr), size(size), retire_epoch(retire_epoch), callback(callback) {}
};

struct ThreadLocalGC {
    std::atomic<int64_t> local_epoch{0};
    std::list<RetiredNode> retired_nodes;

    ThreadLocalGC() = default;
    ThreadLocalGC(int64_t local_epoch) : local_epoch(local_epoch), retired_nodes() {}
    ThreadLocalGC(const ThreadLocalGC&) = delete;
    ThreadLocalGC& operator=(const ThreadLocalGC&) = delete;
};

class EpochBasedGC {
public:
    explicit EpochBasedGC();
    ~EpochBasedGC();

    uint64_t register_thread();
    void retire(size_t tid, void* addr, size_t size, CleanupCallback callback);
    void begin_op(size_t tid);
    void end_op(size_t tid);

    int64_t get_global_epoch() const;
    int64_t get_thread_epoch(size_t tid) const;
    size_t get_retired_count() const;

private:
    std::atomic<bool> running;
    std::atomic<int64_t> global_epoch;
    std::thread gc_thread;
    ThreadLocalGC thread_local_gc[MAX_CID];
    std::atomic<int64_t> registered_threads;

    void gc_loop();
    void try_reclaim(size_t tid);
    void force_cleanup();
};