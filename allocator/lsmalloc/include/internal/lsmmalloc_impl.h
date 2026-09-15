#pragma once

#include "clht_db.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace lsmallocimpl {

constexpr size_t MAX_CID = 1024;

using id_t = uint32_t;
using cid_t = uint32_t;

struct ObjectEntry {
    size_t id;
    std::atomic<void*> addr;
    std::atomic<int> ref_count;
    std::atomic<size_t> size;
    std::atomic<bool> alive;

    ObjectEntry();
    ObjectEntry(const ObjectEntry&) = delete;
    ObjectEntry& operator=(const ObjectEntry&) = delete;
    ObjectEntry(ObjectEntry&&) = delete;
    ObjectEntry& operator=(ObjectEntry&&) = delete;
};

struct SharedAllocatorMeta {
    std::atomic<bool> initialized;
    std::atomic<id_t> next_id;
    std::atomic<size_t> offset;
    std::atomic<size_t> base;
    size_t max_objects;
    size_t log_segment_size;
};

template <typename T>
struct Segment {
    size_t size;
    T data;
};

class LockFreeLogAllocator {
public:
    LockFreeLogAllocator(void* data_shm_path, std::unique_ptr<CLHTDB> clht_db);
    ~LockFreeLogAllocator();

    // allocate and free
    [[deprecated("use allocate<T> instead")]] id_t allocate(size_t size, cid_t cid);
    [[deprecated("use acquire<T> instead")]] void acquire(id_t id, cid_t cid);
    [[deprecated("use release<T> instead")]] void release(id_t id, cid_t cid);

    // read and write
    [[deprecated("use read<T> instead")]] void* read(id_t id, cid_t cid);
    [[deprecated("use write<T> instead")]] void write(id_t id, const void* data, size_t size,
                                                      cid_t cid);
    template <typename T>
    id_t allocate(cid_t cid);
    template <typename T>
    void free(id_t id, cid_t cid);
    template <typename T>
    void acquire(id_t id, cid_t cid);
    template <typename T>
    void release(id_t id, cid_t cid);
    template <typename T>
    T* read(id_t id, cid_t cid);
    template <typename T>
    void write(id_t id, const T& data, cid_t cid);

    size_t get_segment_begin_offset() const;

    // get address
    void* get_addr(id_t id, cid_t cid);

    // register thread
    int register_thread();
    void unregister_thread(int thread_id);
    void display_status(std::ostream& os = std::cout);

    void debug_print_status(size_t object_id) const;

private:
    // data on shared memory
    SharedAllocatorMeta* meta;
    ObjectEntry* entries;
    std::unique_ptr<CLHTDB> clht_db;

    int fd;
};

struct lsmalloc_config {
    const char* data_path;
    size_t data_size;
    const char* hashmap_path;
    size_t hashmap_size;
    size_t max_objects;
    bool truncate;
    int thread_num;
    int num_buckets;
};

std::unique_ptr<LockFreeLogAllocator> init_allocator(lsmalloc_config config);

template <typename T>
T* LockFreeLogAllocator::read(id_t id, cid_t cid) {
    (void)cid;
    uint64_t addr;
    clht_db->Read(id, addr);
    if (addr == 0) {
        std::cerr << "Error get address of object " << id << " from clht_db" << std::endl;
        return nullptr;
    }
    Segment<T>* seg = reinterpret_cast<Segment<T>*>(addr);
    return &(seg->data);
}

template <typename T>
void LockFreeLogAllocator::write(id_t id, const T& data, cid_t cid) {
    (void)cid;
    Segment<T> seg;
    seg.size = sizeof(T);
    seg.data = data;
    size_t size = sizeof(Segment<T>);
    size_t offset = meta->offset.fetch_add(size, std::memory_order_acq_rel);
    if (offset + size > meta->log_segment_size) {
        throw std::runtime_error("Out of memory in segment (CoW)");
    }
    void* new_ptr = (void*)(get_segment_begin_offset() + offset);

    std::memcpy(new_ptr, &seg, size);

    // old for entries
    // entries[id].addr.exchange(new_ptr, std::memory_order_acq_rel);
    // new for clht_db
    clht_db->Insert(id, (uint64_t)new_ptr);
}

template <typename T>
id_t LockFreeLogAllocator::allocate(cid_t cid) {
    static_assert(std::is_copy_constructible<T>::value && std::is_copy_assignable<T>::value,
                  "T must be copy constructible and copy assignable");
    (void)cid;
    id_t id = meta->next_id.fetch_add(1, std::memory_order_acq_rel);
    assert(id < meta->max_objects);
    entries[id].ref_count.store(1, std::memory_order_release);
    entries[id].size = sizeof(T);
    entries[id].addr.store(nullptr, std::memory_order_release);
    return id;
}

template <typename T>
void LockFreeLogAllocator::free(id_t id, cid_t cid) {
    (void)cid;
    entries[id].ref_count.store(0, std::memory_order_release);
    entries[id].addr.store(nullptr, std::memory_order_release);
}

template <typename T>
void LockFreeLogAllocator::acquire(id_t id, cid_t cid) {
    (void)cid;
    entries[id].ref_count.fetch_add(1, std::memory_order_acq_rel);
}

template <typename T>
void LockFreeLogAllocator::release(id_t id, cid_t cid) {
    (void)cid;
    entries[id].ref_count.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace lsmallocimpl
