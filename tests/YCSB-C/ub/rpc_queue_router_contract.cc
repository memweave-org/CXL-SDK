#include "db/rpc_queue_router.h"
#include <iostream>
#include <thread>
#include <vector>

using Router = ycsbc::RpcQueueRouter;
using Policy = Router::Policy;
static void require(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}

int main() {
  // Hold responses from bucket 0, even though its requests were dequeued.
  // New requests must avoid this slow destination until responses are consumed.
  Router router(3, Policy::LeastOutstanding);
  for (int i = 0; i < 3; ++i)
    require(router.try_submit(i, 0, [](size_t q) { return q == 0; }) == 0,
            "could not hold requests on the slow destination");
  for (int i = 0; i < 32; ++i) {
    const auto q = router.try_submit(i, 0, [](size_t) { return true; });
    require(q != 0 && q != Router::no_bucket, "routed to the overloaded queue");
    router.complete(q);
  }
  require(router.outstanding(0) == 3, "lost executing/unconsumed requests");
  const auto failed = router.try_submit(0, 0, [](size_t) { return false; });
  require(failed == Router::no_bucket && router.outstanding(0) == 3 &&
          router.outstanding(1) == 0 && router.outstanding(2) == 0,
          "failed enqueue leaked an outstanding reservation");
  for (int i = 0; i < 3; ++i) router.complete(0);
  require(router.try_submit(0, 0, [](size_t) { return true; }) == 0,
          "drained queue did not become eligible again");
  router.complete(0);

  Router fixed(3, Policy::Fixed), random(3, Policy::Random);
  int attempts = 0;
  require(fixed.try_submit(1, 0, [&](size_t q) {
    ++attempts; require(q == 1, "fixed policy changed client destination"); return false;
  }) == Router::no_bucket && attempts == 1, "fixed policy retried another queue");
  require(random.try_submit(0, 0, [](size_t q) { return q == 2; }) == 2,
          "random policy did not retry another queue");
  random.complete(2);

  // All clients share one router. Check accounting after simultaneous enqueue
  // failures, successful submissions and response consumption.
  std::atomic<bool> start{false};
  std::vector<std::thread> clients;
  for (size_t client = 0; client < 8; ++client) clients.emplace_back([&, client] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    for (size_t i = 0; i < 10000; ++i) {
      const auto q = router.try_submit(client, i % 3, [&](size_t bucket) {
        return (i + bucket) % 3 != 0;
      });
      require(q != Router::no_bucket, "missed available queue");
      router.complete(q);
    }
  });
  start.store(true, std::memory_order_release);
  for (auto& client : clients) client.join();
  for (size_t q = 0; q < router.buckets(); ++q)
    require(router.outstanding(q) == 0, "concurrent accounting leaked requests");
  setenv("RPC_QUEUE_POLICY", "invalid", 1);
  bool rejected = false;
  try { Router::configured_policy(); } catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "invalid policy accepted");
  unsetenv("RPC_QUEUE_POLICY");
  require(Router::configured_policy() == Policy::LeastOutstanding, "wrong default policy");
  std::cout << "PASS RPC routing: slow destination, retry rollback, drain and concurrent accounting\n";
}
