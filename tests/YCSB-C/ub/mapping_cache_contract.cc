// Bounded correctness checks; no throughput/latency measurements. The shared
// channel uses coherent mmap here, not a simulation of non-coherent hardware.
#include "process_mapping_cache.h"
#include <cerrno>
#include <cstdlib>
#include <future>
#include <iostream>
#include <sys/wait.h>

namespace {
void require(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}
void transfer(int fd, void* buffer, size_t bytes, bool writing) {
  auto* cursor = static_cast<char*>(buffer);
  while (bytes) {
    const auto count = writing ? write(fd, cursor, bytes) : read(fd, cursor, bytes);
    if (count < 0 && errno == EINTR) continue;
    require(count > 0, "pipe disconnected");
    cursor += count;
    bytes -= count;
  }
}
void send(int fd, uint64_t value) { transfer(fd, &value, sizeof(value), true); }
uint64_t receive(int fd) {
  uint64_t value;
  transfer(fd, &value, sizeof(value), false);
  return value;
}
template<class Predicate> void eventually(Predicate done) {
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!done()) {
    require(std::chrono::steady_clock::now() < limit, "condition did not complete");
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}
uintptr_t identity(uintptr_t value) { return value; }
uintptr_t refill(MessageMappingCacheSlot& slot, const std::atomic<uintptr_t>& mapping) {
  return slot.fetch_and_fill([&] { return mapping.load(); }, identity);
}

void channel_contract() {
  MappingInvalidationChannel<std::atomic, 2, 2> channel;
  channel.attach(0);
  channel.attach(1);
  MessageMappingCacheSlot a, b;
  std::atomic<uintptr_t> mapping{100};
  require(refill(a, mapping) == 100, "initial A fill");
  mapping.store(200);
  require(refill(b, mapping) == 200, "initial B fill");
  const auto ticket = channel.try_publish(7);
  require(ticket.sequence && !channel.acknowledged(ticket), "premature ACK");
  channel.consume(0, [&](uint64_t id) { require(id == 7, "wrong node"); b.invalidate(); });
  require(!channel.acknowledged(ticket), "one ACK accepted for two recipients");
  channel.consume(1, [&](uint64_t id) { require(id == 7, "wrong node"); a.invalidate(); });
  require(channel.acknowledged(ticket), "missing complete ACK");
  require(a.load() == 0 && b.load() == 0, "cache not cleared");
  require(refill(a, mapping) == 200 && a.load() == 200, "refill after clear used an old pointer");

  const auto first = channel.try_publish(8), second = channel.try_publish(9);
  require(first.sequence && second.sequence, "queue capacity too small");
  require(!channel.try_publish(10).sequence, "full queue overwrote a message");
  uint64_t expected = 8;
  channel.consume(0, [&](uint64_t id) { require(id == expected++, "FIFO broken"); });
  require(!channel.try_publish(10).sequence, "slow receiver was overwritten");
  channel.detach(1); // No remaining readers: retire its entire cache.
  require(channel.acknowledged(second), "detach did not retire pending ACKs");
  const auto third = channel.try_publish(10);
  require(third.sequence && third.recipients == 1, "detached process still targeted");
  channel.attach(1); // A fresh empty table skips messages published before attach.
  require(!channel.consume(1, [](uint64_t) {}), "new consumer saw old messages");
  channel.consume(0, [](uint64_t id) { require(id == 10, "ring reuse lost message"); });
  require(channel.acknowledged(third) && channel.acknowledged(second), "ACK regressed on reattach");
  channel.detach(0);
  channel.detach(1);
}

void refill_contract() {
  MappingInvalidationChannel<std::atomic, 2, 1> channel;
  channel.attach(0);
  MessageMappingCacheSlot slot;
  std::atomic<uintptr_t> mapping{100};
  std::promise<void> fetched, resume;
  auto fetched_future = fetched.get_future(), resume_future = resume.get_future();
  uintptr_t result = 0;
  std::thread filling([&] {
    result = slot.fetch_and_fill([&] {
      const auto old = mapping.load();
      fetched.set_value(); // Hold busy after fetching P, before publishing it.
      resume_future.wait();
      return old;
    }, identity);
  });
  fetched_future.wait();
  require(slot.load() == 0, "busy marker exposed as a cached pointer");
  mapping.store(200);
  const auto ticket = channel.try_publish(7);
  std::promise<void> clearing;
  auto clearing_future = clearing.get_future();
  std::thread receiver([&] {
    channel.consume(0, [&](uint64_t id) {
      require(id == 7, "wrong node");
      clearing.set_value();
      slot.invalidate();
    });
  });
  clearing_future.wait();
  require(!channel.acknowledged(ticket), "ACK before the in-flight fill finished");
  bool encoded = false;
  require(slot.fetch_and_fill([&] { return mapping.load(); }, [&](uintptr_t value) {
    encoded = true; return value;
  }) == 200 && !encoded, "busy reader waited or attempted to publish");
  // No timing assumption: the filling thread is still gated on this promise.
  resume.set_value();
  filling.join();
  receiver.join();
  require(result == 100 && channel.acknowledged(ticket), "fill/ACK ordering");
  require(slot.load() == 0, "delayed fill survived its invalidation");
  require(refill(slot, mapping) == 200 && slot.load() == 200, "fresh fill after ACK");

  // Exceptions must release busy, allowing subsequent clears and refills.
  bool threw = false;
  try {
    slot.fetch_and_fill([]() -> uintptr_t { throw std::runtime_error("fetch"); }, identity);
  } catch (const std::runtime_error&) { threw = true; }
  require(threw && slot.load() == 0, "failed fetch left slot busy");
  slot.invalidate();
  require(refill(slot, mapping) == 200, "refill after fetch exception");
  threw = false;
  try {
    slot.fetch_and_fill([&] { return mapping.load(); }, [](uintptr_t) -> uintptr_t {
      throw std::runtime_error("encode");
    });
  } catch (const std::runtime_error&) { threw = true; }
  require(threw && slot.load() == 0, "failed encode left slot busy");
  slot.invalidate();
  require(refill(slot, mapping) == 200, "refill after encode exception");
  channel.detach(0);
}

using Cache = ProcessMappingCache<16, std::atomic>;
struct Shared {
  Cache::Channel channel;
  std::atomic<uintptr_t> mapping{100};
};
constexpr size_t entry = 7;

void process_contract() {
  auto* shared = static_cast<Shared*>(mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE,
                                          MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  require(shared != MAP_FAILED, "shared mmap");
  new (shared) Shared;
  int to_child[2], to_parent[2];
  require(pipe(to_child) == 0 && pipe(to_parent) == 0, "pipe");
  const auto child = fork(); // Fork before starting either process's receiver.
  require(child >= 0, "fork");
  if (child == 0) {
    close(to_child[1]); close(to_parent[0]);
    try {
      Cache::prepare(shared, shared->channel, 1);
      auto& slot = Cache::table(shared)[entry];
      require(refill(slot, shared->mapping) == 100, "A fill");
      send(to_parent[1], 1);
      require(receive(to_child[0]) == 1, "start refill command");
      send(to_parent[1], slot.load()); // B's private fill must not change A's hit.
      std::promise<void> fetched, resume;
      auto fetched_future = fetched.get_future(), resume_future = resume.get_future();
      uintptr_t result = 0;
      std::thread filling([&] {
        result = slot.fetch_and_fill([&] {
          const auto value = shared->mapping.load();
          fetched.set_value();
          resume_future.wait();
          return value;
        }, identity);
      });
      fetched_future.wait();
      send(to_parent[1], 2); // A now holds busy and the old authoritative pointer.
      require(receive(to_child[0]) == 2, "finish refill command");
      bool encoded = false;
      require(slot.fetch_and_fill([&] { return shared->mapping.load(); }, [&](uintptr_t value) {
        encoded = true; return value;
      }) == 400 && !encoded, "A busy reader failed to bypass the pending fill");
      resume.set_value();
      filling.join();
      require(result == 200, "A's held snapshot unexpectedly changed");
      send(to_parent[1], 2);
      require(receive(to_child[0]) == 3, "check clear command");
      require(slot.load() == 0, "ACK did not clear A's delayed fill");
      require(refill(slot, shared->mapping) == 400, "A fresh fill");
      send(to_parent[1], 3);
      require(receive(to_child[0]) == 4, "exit command");
      require(slot.load() == 400, "B changed A's private table");
      bwtree_process_cache::shutdown();
      require(munmap(shared, sizeof(Shared)) == 0, "A unmap");
      // A's receiver must have joined before its shared mapping disappears.
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      send(to_parent[1], 4);
      _exit(0);
    } catch (const std::exception& error) {
      std::cerr << "A: " << error.what() << '\n';
      _exit(1);
    }
  }
  close(to_child[0]); close(to_parent[1]);
  bool child_running = true;
  try {
    require(receive(to_parent[0]) == 1, "A attach");
    Cache::prepare(shared, shared->channel, 0);
    auto* table = &Cache::table(shared);
    std::thread worker([&] {
      Cache::prepare(shared, shared->channel, 0);
      require(&Cache::table(shared) == table, "multiple tables in one process");
    });
    worker.join();
    auto& slot = (*table)[entry];
    shared->mapping.store(200);
    require(refill(slot, shared->mapping) == 200, "B fill");
    send(to_child[1], 1);
    require(receive(to_parent[0]) == 100, "B fill changed A cache");
    require(receive(to_parent[0]) == 2, "A did not start its pending fill");
    shared->mapping.store(400);
    const auto ticket = shared->channel.try_publish(entry);
    require(ticket.sequence && ticket.recipients == 3, "broadcast missing recipient");
    eventually([&] { return slot.load() == 0; }); // B has consumed its own clear.
    require(!shared->channel.acknowledged(ticket), "A ACKed while holding a pending fill");
    send(to_child[1], 2);
    require(receive(to_parent[0]) == 2, "A busy-reader check");
    eventually([&] { return shared->channel.acknowledged(ticket); });
    send(to_child[1], 3);
    require(receive(to_parent[0]) == 3, "A clear/fresh-fill check");
    shared->mapping.store(500);
    require(refill(slot, shared->mapping) == 500, "B fresh fill");
    const auto unrelated = shared->channel.try_publish(entry + 1);
    eventually([&] { return shared->channel.acknowledged(unrelated); });
    require(slot.load() == 500, "unrelated message cleared whole table");
    send(to_child[1], 4);
    require(receive(to_parent[0]) == 4, "A stop before unmap");
    int status;
    require(waitpid(child, &status, 0) == child, "waitpid");
    child_running = false;
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "A process failed");
    Cache::release(shared);
    Cache::prepare(shared, shared->channel, 0); // Same address, fresh table/binding.
    require(Cache::table(shared)[entry].load() == 0, "retired table reused");
    bwtree_process_cache::shutdown();
    shared->~Shared();
    require(munmap(shared, sizeof(Shared)) == 0, "B unmap");
    close(to_child[1]); close(to_parent[0]);
  } catch (...) {
    if (child_running) {
      kill(child, SIGKILL); // Only this test's own, unreaped child.
      waitpid(child, nullptr, 0);
    }
    bwtree_process_cache::shutdown();
    throw;
  }
}
} // namespace

int main() {
  alarm(20);
  signal(SIGPIPE, SIG_IGN);
  try {
    channel_contract();
    refill_contract();
    process_contract();
    std::cout << "PASS: private tables, fill/clear exclusion, busy-reader bypass, ACK, backpressure, detach/reuse\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
