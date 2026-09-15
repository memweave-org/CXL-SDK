#include "internal/lsmmalloc_impl.h"

#include "clht_db.h"
#include "lsmalloc_allocator.h"
#include "utils/log.h"
#include "utils/units.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>

using namespace lsmallocimpl;

ObjectEntry::ObjectEntry() : id(0), addr(nullptr), ref_count(0), size(0) {}

std::unique_ptr<LockFreeLogAllocator> lsmallocimpl::init_allocator(lsmalloc_config config) {
    const char* data_path = config.data_path;
    const char* hashmap_path = config.hashmap_path;
    size_t data_size = config.data_size;
    size_t hashmap_size = config.hashmap_size;
    size_t max_objects = config.max_objects;
    bool truncate = config.truncate;

    int fd = open(data_path, O_RDWR, 0666);
    if (fd < 0) {
        throw std::runtime_error("Failed to open data device: " + std::string(data_path));
    }

    if (truncate) {
        ftruncate(fd, data_size);
    }

    char* data_mapped_base =
        (char*)mmap(nullptr, data_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(data_mapped_base != MAP_FAILED);
    close(fd);

    fd = open(hashmap_path, O_RDWR, 0666);
    if (fd < 0) {
        throw std::runtime_error("Failed to open hashmap device: " + std::string(hashmap_path));
    }

    if (truncate) {
        ftruncate(fd, hashmap_size);
    }
    char* hashmap_mmap_base =
        (char*)mmap(nullptr, hashmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(hashmap_mmap_base != MAP_FAILED);
    close(fd);

    SimThreadInfo::worker_machine_count = 1;
    SimThreadInfo::worker_machine_id = 0;

    init_cxl_cacheable_allocator(hashmap_path, hashmap_mmap_base, hashmap_size);

    std::unique_ptr<CLHTDB> clht_db =
        std::make_unique<CLHTDB>(config.thread_num, config.num_buckets);

    size_t meta_size = sizeof(SharedAllocatorMeta);
    size_t entries_size = sizeof(ObjectEntry) * max_objects;
    size_t log_segment_size = data_size - meta_size - entries_size;

    // [meta] [entries] [log_segment]

    SharedAllocatorMeta* meta = reinterpret_cast<SharedAllocatorMeta*>(data_mapped_base);

    ObjectEntry* entries =
        reinterpret_cast<ObjectEntry*>(data_mapped_base + sizeof(SharedAllocatorMeta));

    for (size_t i = 0; i < max_objects; ++i) {
        new (&entries[i]) ObjectEntry();  // initialize all entries
    }
    if (truncate || meta->initialized.load() == false) {
        meta->next_id.store(0, std::memory_order_relaxed);
        meta->initialized.store(true, std::memory_order_relaxed);
        meta->max_objects = max_objects;
        meta->offset.store(0, std::memory_order_relaxed);
        meta->base.store((uint64_t)data_mapped_base, std::memory_order_relaxed);
        meta->log_segment_size = log_segment_size;
    }

    LOG_INFO("init allocator: at {}, total_size: {}, max_objects: {}, log_segment_size: {}\n",
             data_path, units::byte_format(data_size), max_objects,
             units::byte_format(log_segment_size));

    // TODO: start the bg gc thread
    std::unique_ptr<LockFreeLogAllocator> allocator =
        std::make_unique<LockFreeLogAllocator>(data_mapped_base, std::move(clht_db));
    return allocator;
}

std::unique_ptr<LockFreeLogAllocator> lsmalloc::init_allocator(lsmalloc_config config) {
    return lsmallocimpl::init_allocator(config);
}

LockFreeLogAllocator::LockFreeLogAllocator(void* data_shm_path, std::unique_ptr<CLHTDB> clht_db) {
    meta = reinterpret_cast<SharedAllocatorMeta*>((size_t)data_shm_path);
    entries = reinterpret_cast<ObjectEntry*>((size_t)data_shm_path + sizeof(SharedAllocatorMeta));
    if (meta->initialized.load() == false) {
        throw std::runtime_error("LockFreeLogAllocator not initialized");
    }
    this->clht_db = std::move(clht_db);
    // display_status();
}

LockFreeLogAllocator::~LockFreeLogAllocator() {}

size_t LockFreeLogAllocator::get_segment_begin_offset() const {
    return meta->base.load(std::memory_order_acquire) + sizeof(SharedAllocatorMeta) +
           sizeof(ObjectEntry) * meta->max_objects;
}

int LockFreeLogAllocator::register_thread() {
    int thread_id = clht_db->PoolThreadInit();
    return thread_id;
}

void LockFreeLogAllocator::unregister_thread(int thread_id) {
    clht_db->PoolThreadClose(thread_id);
}

void LockFreeLogAllocator::display_status(std::ostream& os) {
    size_t total_size = meta->log_segment_size + sizeof(SharedAllocatorMeta) +
                        sizeof(ObjectEntry) * meta->max_objects;
    size_t used = meta->offset.load(std::memory_order_acquire);
    size_t free_space = meta->log_segment_size - used;
    size_t max_objects = meta->max_objects;
    size_t allocated_objects = meta->next_id.load(std::memory_order_acquire);
    size_t log_segment_size = meta->log_segment_size;
    size_t output_count =
        &os == &std::cout ? std::min((size_t)10, allocated_objects) : allocated_objects;

    std::stringstream ss;
    ss << "\n============ LockFreeLogAllocator Status =============\n";
    ss << "Total size: " << units::byte_format(total_size) << "\n";
    ss << "Max objects: " << max_objects << "\n";
    ss << "Log segment size: " << units::byte_format(log_segment_size) << "\n";
    ss << "Used: " << units::byte_format(used) << "\n";
    ss << "Free: " << units::byte_format(free_space) << "\n";
    ss << "Allocated objects: " << allocated_objects << "\n";
    ss << "Object status:\n";
    ss << "ID\tRef\tAddr\t\t\tSize\tAlive\n";
    ss << "--------------------------------------------------------------\n";
    for (size_t i = 0; i < output_count; ++i) {
        void* addr = entries[i].addr.load(std::memory_order_acquire);
        size_t size = entries[i].size;
        bool alive = entries[i].alive;
        ss << i << "\t" << entries[i].ref_count.load(std::memory_order_acquire) << "\t0x" << addr
           << "\t\t\t" << units::byte_format(size) << "\t" << (alive ? "true" : "false") << "\n";
    }
    ss << "==============================================================\n";
    os << ss.str();
}

void* LockFreeLogAllocator::get_addr(id_t id, cid_t cid) {
    (void)cid;
    return entries[id].addr.load(std::memory_order_acquire);
}

[[deprecated("use allocate<T> instead")]] id_t LockFreeLogAllocator::allocate(size_t size,
                                                                              cid_t cid) {
    (void)cid;
    id_t id = meta->next_id.fetch_add(1, std::memory_order_acq_rel);
    assert(id < meta->max_objects);
    entries[id].ref_count.store(1, std::memory_order_release);
    entries[id].size = size;
    entries[id].addr.store(nullptr, std::memory_order_release);
    return id;
}

[[deprecated("use acquire<T> instead")]] void LockFreeLogAllocator::acquire(id_t id, cid_t cid) {
    (void)cid;
    int current_ref_count = entries[id].ref_count.load(std::memory_order_acquire);
    int current_next_id = meta->next_id.load(std::memory_order_acquire);
    if (current_ref_count == 0 && current_next_id >= (int)id) {
        if (entries[id].ref_count.compare_exchange_strong(
                current_ref_count, 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            meta->next_id.store(id + 1, std::memory_order_release);
        } else {
            entries[id].ref_count.fetch_add(1, std::memory_order_acq_rel);
        }
    } else {
        entries[id].ref_count.fetch_add(1, std::memory_order_acq_rel);
    }
}

[[deprecated("use release<T> instead")]] void LockFreeLogAllocator::release(id_t id, cid_t cid) {
    (void)cid;
    entries[id].ref_count.fetch_sub(1, std::memory_order_acq_rel);
}

[[deprecated("use read<T> instead")]] void* LockFreeLogAllocator::read(id_t id, cid_t cid) {
    (void)cid;
    entries[id].ref_count.fetch_add(1, std::memory_order_acq_rel);
    void* addr = entries[id].addr.load(std::memory_order_acquire);
    entries[id].ref_count.fetch_sub(1, std::memory_order_acq_rel);
    return addr;
}

[[deprecated("use write<T> instead")]] void LockFreeLogAllocator::write(id_t id, const void* data,
                                                                        size_t size, cid_t cid) {
    (void)cid;
    // Always perform Copy-on-Write (CoW), never write in place
    int ref_count = entries[id].ref_count.load(std::memory_order_acquire);
    void* old_addr = entries[id].addr.load(std::memory_order_acquire);

    // Allocate new block
    size_t offset = meta->offset.fetch_add(size, std::memory_order_acq_rel);
    if (offset + size > meta->log_segment_size) {
        throw std::runtime_error("Out of memory in segment (CoW)");
    }
    size_t base = meta->base.load(std::memory_order_acquire);
    void* new_ptr = (void*)(base + offset);

    if (data) {
        std::memcpy(new_ptr, data, size);
    } else {
        std::memset(new_ptr, 0, size);
    }
    // Atomically update addr
    entries[id].addr.exchange(new_ptr, std::memory_order_acq_rel);
    if (entries[id].size == 0) {
        entries[id].size = size;
    }
    // Decrement old ref_count if necessary
    if (ref_count > 1 && old_addr != nullptr) {
        entries[id].ref_count.fetch_sub(1, std::memory_order_acq_rel);
        // Optionally, add old_addr to dead_objects for GC
        // dead_objects.push_back({old_addr, size});
    }
}
