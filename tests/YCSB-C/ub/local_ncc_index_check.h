#pragma once

#include <set>
#include "core/db.h"
#include "shm/mm.h"
#include "utils/bypass_cache.h"
#include "utils/sim_id.h"

namespace ycsbc {
// Stored in the controller's NC allocation. Only the verifier protocol is
// shared; it never invalidates CACHE lines or repairs the index being tested.
struct LocalNccIndexCheck {
  nt<uint64_t> published{0}, acknowledged{0};
  uint64_t expected_value = 0;

  template<class Groups>
  size_t run(DB* db, const Groups& groups) {
    const bool writer = SimThreadInfo::is_master();
    const auto mid = SimThreadInfo::worker_machine_id;
    SimThreadInfo::setup_worker_ids(mid, ycsbc_ub::first_thread(mid));
    const int id = db->ThreadInit();
    db->Init();
    std::set<uint64_t> keys;
    for (const auto& group : groups)
      for (const auto& op : group) keys.insert(op.key);
    uint64_t step = 0;
    auto verify = [&](uint64_t key, unsigned version) {
      ++step;
      if (writer) {
        uintptr_t value = 0;
        if (db->ReadInternal(key, value) != DB::kOK || !value)
          throw std::runtime_error("LOCAL_NCC_VERIFY: B cannot read published key");
        if (version && value != expected_value)
          throw std::runtime_error("LOCAL_NCC_VERIFY: update did not replace B's value");
        expected_value = value;
        published.store(step, std::memory_order_release);
        while (acknowledged.load(std::memory_order_acquire) != step) std::this_thread::yield();
      } else {
        while (published.load(std::memory_order_acquire) != step) std::this_thread::yield();
        uintptr_t value = 0;
        if (db->ReadInternal(key, value) != DB::kOK || value != expected_value)
          throw std::runtime_error("LOCAL_NCC_VERIFY: A has missing or outdated value pointer");
        if (version) {
          const auto* payload = reinterpret_cast<const unsigned char*>(value);
          for (size_t i = 0; i < VALUE_ADDR_SIZE; ++i)
            if (payload[i] != static_cast<unsigned char>(version + i))
              throw std::runtime_error("LOCAL_NCC_VERIFY: A has incorrect updated payload");
        }
        acknowledged.store(step, std::memory_order_release);
      }
    };
    // Once both workloads have stopped, every touched key must resolve to the
    // same final value on A and B after B updates; A never mutates the index.
    for (auto key : keys) verify(key, 0);
    // Distinct payloads make successful-but-no-op updates detectable. A reads
    // each published version before B can publish the next one.
    for (unsigned version = 1; version <= 32; ++version) {
      if (writer) {
        auto* payload = static_cast<unsigned char*>(cacheable.malloc(VALUE_ADDR_SIZE));
        for (size_t i = 0; i < VALUE_ADDR_SIZE; ++i)
          payload[i] = static_cast<unsigned char>(version + i);
        clwb(payload, VALUE_ADDR_SIZE);
        expected_value = reinterpret_cast<uintptr_t>(payload);
        if (db->UpdateInternal(*keys.begin(), expected_value) != DB::kOK)
          throw std::runtime_error("LOCAL_NCC_VERIFY: update failed");
      }
      verify(*keys.begin(), version);
    }
    db->ThreadClose(id);
    return keys.size();
  }
};
} // namespace ycsbc
