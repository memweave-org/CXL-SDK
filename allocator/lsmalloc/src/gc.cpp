#include "internal/gc.h"

#include <chrono>
#include <cstring>

EpochBasedGC::EpochBasedGC() : running(true), global_epoch(0), registered_threads(0) {
    gc_thread = std::thread([this]() { this->gc_loop(); });
}

EpochBasedGC::~EpochBasedGC() {
    running.store(false);
    if (gc_thread.joinable())
        gc_thread.join();
    // 程序退出时，强制清理所有剩余的退休项
    force_cleanup();
}

uint64_t EpochBasedGC::register_thread() {
    auto tid = registered_threads.fetch_add(1, std::memory_order_relaxed);
    thread_local_gc[tid].local_epoch.store(INT64_MAX, std::memory_order_relaxed);
    return tid;
}

int64_t EpochBasedGC::get_global_epoch() const {
    return global_epoch.load(std::memory_order_acquire);
}

int64_t EpochBasedGC::get_thread_epoch(size_t tid) const {
    return thread_local_gc[tid].local_epoch.load(std::memory_order_acquire);
}

size_t EpochBasedGC::get_retired_count() const {
    size_t count = 0;
    for (int64_t tid = 0; tid < registered_threads; ++tid) {
        count += thread_local_gc[tid].retired_nodes.size();
    }
    return count;
}

// 非阻塞关键点 3: gc_loop 是唯一执行清理的地方
void EpochBasedGC::gc_loop() {
    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(GC_CHECK_PERIOD_MS));

        uint64_t new_epoch = global_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;

        for (int64_t i = 0; i < registered_threads; ++i) {
            thread_local_gc[i].local_epoch.store(new_epoch, std::memory_order_release);
        }
    }
}

void EpochBasedGC::try_reclaim(size_t tid) {
    int64_t min_epoch = INT64_MAX;
    for (int64_t i = 0; i < registered_threads; ++i) {
        // LOG_INFO("try_reclaim: tid: {}, thread_local_gc: {}", i,
        //          thread_local_gc[i].local_epoch.load(std::memory_order_relaxed));
        min_epoch = std::min(
            min_epoch, (int64_t)thread_local_gc[i].local_epoch.load(std::memory_order_relaxed));
    }

    int64_t safe_epoch = (min_epoch > 0) ? min_epoch - 1 : 0;

    auto& retired = thread_local_gc[tid].retired_nodes;
    auto it = retired.begin();
    while (it != retired.end()) {
        if ((int64_t)it->retire_epoch < safe_epoch) {
            // Safe to reclaim
            it->callback(it->addr, it->size);
            it = retired.erase(it);
        } else {
            ++it;
        }
    }
}

void EpochBasedGC::begin_op(size_t tid) {
    thread_local_gc[tid].local_epoch.store(global_epoch.load(std::memory_order_relaxed),
                                           std::memory_order_release);
}

void EpochBasedGC::retire(size_t tid, void* addr, size_t size, CleanupCallback callback) {
    thread_local_gc[tid].retired_nodes.push_back(
        RetiredNode(addr, size, global_epoch.load(std::memory_order_relaxed), callback));
}

void EpochBasedGC::end_op(size_t tid) {
    thread_local_gc[tid].local_epoch.store(global_epoch.load(std::memory_order_relaxed),
                                           std::memory_order_release);
    try_reclaim(tid);
}

void EpochBasedGC::force_cleanup() {
    for (int64_t tid = 0; tid < registered_threads; ++tid) {
        try_reclaim(tid);
    }
}
