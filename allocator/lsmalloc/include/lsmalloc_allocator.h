#pragma once

#include "clht_db.h"
#include "internal/lsmmalloc_impl.h"

#include <cstddef>

namespace lsmalloc {

using id_t = uint32_t;
using cid_t = uint32_t;

class lsmallocator;

// type-safe SHMRef, support arbitrary size memory block
// when T=void, size_ must be specified, otherwise it is sizeof(T)
template <typename T>
class SHMRef {
    id_t id;
    const lsmallocator* allocator_;

public:
    SHMRef(const lsmallocator* allocator, id_t id) : id(id), allocator_(allocator) {}
    id_t get_id() const { return id; }
    size_t size() const { return sizeof(T); }
    T* read() const;
    void write(const T& data) const;
};

// 针对 void 的特化
// template <>
// class SHMRef<void> {
//     id_t id;
//     size_t size_;
//     const lsmallocator* allocator_;

// public:
//     SHMRef(const lsmallocator* allocator, id_t id, size_t size)
//         : id(id), size_(size), allocator_(allocator) {}
//     id_t get_id() const { return id; }
//     size_t size() const { return size_; }
//     void* read() const;
//     void write(const void* data) const;
// };

// allocator instance, can be multiple instances, but operate on the same g_allocator
class lsmallocator {
public:
    lsmallocator(void* shm_base, cid_t cid) : cid(cid) {
        // TODO: no constructor for LockFreeLogAllocator
        // allocator = new lsmallocimpl::LockFreeLogAllocator(shm_base);
        // allocator->register_thread();
    }
    ~lsmallocator() = default;

    // type-safe object allocation
    template <typename T>
    SHMRef<T> alloc() const {
        id_t id = allocator->allocate<T>(cid);
        return SHMRef<T>(this, id);
    }
    // type-safe free
    template <typename T>
    void free(const SHMRef<T>& ref) const {
        allocator->release<T>(ref.get_id(), cid);
    }
    // type-safe read and write
    template <typename T>
    T* read(const SHMRef<T>& ref) const {
        return allocator->read<T>(ref.get_id(), cid);
    }
    template <typename T>
    void write(const SHMRef<T>& ref, const T& data) const {
        allocator->write<T>(ref.get_id(), data, cid);
    }

private:
    cid_t cid;
    lsmallocimpl::LockFreeLogAllocator* allocator;
};

// SHMRef 成员函数实现
template <typename T>
T* SHMRef<T>::read() const {
    return reinterpret_cast<T*>(allocator_->read(*this));
}
template <typename T>
void SHMRef<T>::write(const T& data) const {
    allocator_->write(*this, (void*)&data);
}

// // void 特化实现
// inline void* SHMRef<void>::read() const {
//     return allocator_->read<void>(*this);
// }
// inline void SHMRef<void>::write(const void* data) const {
//     allocator_->write<void>(*this, data);
// }

std::unique_ptr<lsmallocimpl::LockFreeLogAllocator> init_allocator(
    lsmallocimpl::lsmalloc_config config);
}  // namespace lsmalloc
