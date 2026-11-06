#pragma once

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

namespace ycsbc {
// One router per sending process, shared by its client threads. Keep these
// counters in private DRAM: scheduling must not add shared NC atomic traffic.
// Outstanding includes queued, executing and not-yet-consumed responses.
class RpcQueueRouter {
 public:
  enum class Policy { Fixed, Random, LeastOutstanding };
  static constexpr size_t no_bucket = size_t(-1);

  static Policy configured_policy() {
    const char* setting = std::getenv("RPC_QUEUE_POLICY");
    const std::string value = setting ? setting : "least-outstanding";
    if (value == "fixed") return Policy::Fixed;
    if (value == "random") return Policy::Random;
    if (value == "least-outstanding") return Policy::LeastOutstanding;
    throw std::invalid_argument("RPC_QUEUE_POLICY must be fixed, random or least-outstanding");
  }
  static const char* name(Policy policy) {
    switch (policy) {
      case Policy::Fixed: return "fixed";
      case Policy::Random: return "random";
      case Policy::LeastOutstanding: return "least-outstanding";
    }
    std::abort();
  }

  RpcQueueRouter(size_t buckets, Policy policy)
      : buckets_(buckets), policy_(policy), counts_(new Count[buckets]) {
    if (!buckets) throw std::invalid_argument("Empty RPC queue router");
  }
  size_t buckets() const { return buckets_; }
  Policy policy() const { return policy_; }
  size_t outstanding(size_t bucket) const {
    return counts_[bucket].value.load(std::memory_order_relaxed);
  }

  // Fixed uses the client ID; other policies use a caller-supplied random start
  // to spread equal-load ties. A full/contended queue claims no ticket: try the
  // other queues, then return to the caller without retaining a reservation.
  template<class Enqueue>
  size_t try_submit(size_t client, size_t start, Enqueue&& enqueue) {
    size_t first = (policy_ == Policy::Fixed ? client : start) % buckets_;
    if (policy_ == Policy::LeastOutstanding) {
      size_t least = outstanding(first);
      for (size_t offset = 1; least && offset < buckets_; ++offset) {
        const size_t candidate = (start + offset) % buckets_;
        const size_t count = outstanding(candidate);
        if (count < least) { first = candidate; least = count; }
      }
    }
    const size_t attempts = policy_ == Policy::Fixed ? 1 : buckets_;
    for (size_t offset = 0; offset < attempts; ++offset) {
      const size_t bucket = (first + offset) % buckets_;
      if (policy_ == Policy::LeastOutstanding)
        counts_[bucket].value.fetch_add(1, std::memory_order_relaxed);
      if (enqueue(bucket)) return bucket;
      if (policy_ == Policy::LeastOutstanding) complete(bucket);
    }
    return no_bucket;
  }
  void complete(size_t bucket) {
    if (policy_ == Policy::LeastOutstanding) {
      const auto before = counts_[bucket].value.fetch_sub(1, std::memory_order_relaxed);
      assert(before != 0);
      (void)before;
    }
  }

 private:
  struct alignas(64) Count { std::atomic<size_t> value{0}; };
  const size_t buckets_;
  const Policy policy_;
  std::unique_ptr<Count[]> counts_;
};
} // namespace ycsbc
