#pragma once
#include <cstddef>
#include <cstdint>
namespace cxl_transport {
// Non-overlapping copy with streaming stores and safe alignment handling.
void nt_memcpy(void* dst, const void* src, size_t bytes, bool atomic_pairs = false);
void configure_shared_sequences(void* storage);
size_t shared_sequences_bytes();
void* allocate(size_t bytes);
// Shared CAS/lock storage: NC in split UB, original allocator otherwise.
void* allocate_atomic(size_t bytes);
void release(void* p);
void read(void* local, const void* shared, size_t bytes, bool atomic_pairs = false);
void write(void* shared, const void* local, size_t bytes);
// NT copies without explicit cache flushes; retain sequence-validated snapshots.
// Streaming stores do not guarantee uncached source reads on WB memory.
// Ordinary copies for the explicitly UC-mapped Sherman arena.
void* allocate_wb_nt_locks(size_t bytes,int node);
uint64_t nt_lock_load(void* p);
void nt_lock_store(void* p,uint64_t v);
bool nt_lock_cas(void* p,uint64_t& expected,uint64_t desired);
void configure_dma(int fd, void* arena, size_t bytes, int engine_node);
void read_dma(void* local, const void* shared, size_t bytes);
void write_dma(void* shared, const void* local, size_t bytes);
void read_plain(void* local, const void* shared, size_t bytes);
void write_plain(void* shared, const void* local, size_t bytes);
// Single-process WB projection: callback may only read this range and overwrite
// its private output. It can be retried; no externally visible side effects.
void read_projected(const void* shared, size_t bytes, void* output,
                    void (*project)(const void*, void*));
void read_nt(void* local, const void* shared, size_t bytes);
void write_nt(void* shared, const void* local, size_t bytes);
uint64_t load64(const void* p);
void store64(void* p, uint64_t value);
bool cas64(void* p, uint64_t& expected, uint64_t desired);
bool cas128_snapshot(void* p, uint64_t expected[2], const uint64_t desired[2]);
bool cas64_snapshot(void* p, uint64_t& expected, uint64_t desired);
bool cas128(void* p, uint64_t expected[2], const uint64_t desired[2]);
}
