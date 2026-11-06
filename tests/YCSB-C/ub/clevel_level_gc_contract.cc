// Untimed, small file-backed SDK assertions; no hardware/performance claims.
#include "clevel_hash.hpp"
#include <sys/wait.h>
#include <unistd.h>
#include <stdexcept>
#include <iostream>

#ifndef CLEVEL_UB_LEVEL_GC
#error This contract requires UBSM split level GC
#endif
using Table = clevel_hash<uint64_t, uint64_t, std::hash<uint64_t>, std::equal_to<uint64_t>, 4>;
static void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
template<class Predicate> static void wait_for(Predicate predicate, const char* message) {
  for (int i = 0; i < 3000; ++i) {
    if (predicate()) return;
    usleep(1000);
  }
  throw std::runtime_error(message);
}
static Table::level_meta* root(Table* table) {
#ifdef OPT_CLEVEL_ROOT_READ
  auto* m = table->help_update->load_ptr(SimThreadInfo::worker_thread_id);
  require(!ub_arena_owns(true, m), "OPT metadata must not live in NC");
  return m;
#else
  return table->meta.load();
#endif
}
static void stop_resize(Table* table) {
  table->run_expand_thread.store(false);
  if (table->expand_thread.joinable()) table->expand_thread.join();
}
static void migrate(Table* table) {
  table->run_expand_thread.store(true);
  table->expand_thread = std::thread([table] { table->resize(); });
  wait_for([&] {
    ClevelLevelEpochGuard guard(table, table->level_epoch, table->worker_level_epoch(0));
    auto* m = root(table);
    return m->last_level->up.load() == m->first_level && !m->is_resizing;
  }, "migration did not finish");
  stop_resize(table);
}
static Table* make_table(char* data, char* nc, size_t nc_bytes) {
  ub_configure_arena(false, data, 32 * 1024);
  ub_configure_arena(true, nc, nc_bytes);
  cacheable = MemoryManager(false);
  SimThreadInfo::setup_worker_machine_count(2);
  SimThreadInfo::setup_worker_nr(5);
  SimThreadInfo::setup_worker_db_count(1);
  SimThreadInfo::setup_worker_ids(0, 0);
  auto* table = new (shared_atomic_memory().malloc(sizeof(Table))) Table;
  table->set_thread_num(5);
  stop_resize(table);
#ifdef OPT_CLEVEL_ROOT_READ
  require(ub_arena_owns(false, table->meta.load()), "initial metadata must be cacheable");
  require(root(table)->level_count == 2, "initial metadata was not fully initialized");
  require(ub_arena_owns(false, table->help_update), "replica descriptor must be cacheable");
  require(ub_arena_owns(true, &table->meta), "global pointer must remain NC");
  require(ub_arena_owns(true, table->meta_replicas.data()), "replica pointers must remain NC");
  require(table->help_update->load_ptr(0) == table->meta.load(), "cacheable descriptor read wrong root");
#else
  require(ub_arena_owns(true, table->meta.load()), "NOOPT metadata placement changed");
#endif
  for (uint64_t key = 1; key <= 16; ++key) table->insert({key, key + 100}, 0, key);
  migrate(table);
  // Any initial OPT expansion is allowed to finish reclamation first.
  usleep(30000);
  return table;
}
static void expand(Table* table) {
  ClevelLevelEpochGuard guard(table, table->level_epoch, table->worker_level_epoch(0));
  table->expand(0, root(table));
}
static void repeated_growth(char* data, char* nc) {
  auto* table = make_table(data, nc, 4 * 1024 * 1024);
  auto expected_cache_bytes = ub_arena_live(false);
  std::atomic<bool> reading{true};
  std::atomic<unsigned> errors{0};
  std::thread reader([&] {
    SimThreadInfo::setup_worker_ids(0, 2);
    while (reading.load()) {
      for (uint64_t key = 1; key <= 16; ++key) {
        auto found = table->search(key);
        if (!found.found || found.value->second != key + 100) errors.fetch_add(1);
      }
      usleep(1000);
    }
  });
  for (int round = 0; round < 7; ++round) {
    size_t retired_bytes, pinned_bytes;
    {
      ClevelLevelEpochGuard slow_reader(table, table->level_epoch, table->worker_level_epoch(1));
      const auto entered = table->worker_level_epoch(1).epoch.load();
      auto* old = root(table)->last_level;
      retired_bytes = old->capacity * sizeof(Table::bucket) + ((sizeof(Table::level_bucket) + 63) & ~size_t(63));
      expand(table); migrate(table);
#ifdef OPT_CLEVEL_ROOT_READ
      // Expansion and migration each publish one retained immutable image.
      expected_cache_bytes += 2 * ((sizeof(Table::level_meta) + 63) & ~size_t(63));
      require(ub_arena_owns(false, root(table)), "replacement metadata must be cacheable");
#endif
      wait_for([&] { return table->level_epoch.load() > entered; }, "background GC did not advance epoch");
      pinned_bytes = ub_arena_live(true);
      usleep(30000);
      require(ub_arena_live(true) == pinned_bytes, "old level freed while reader still active");
      require(old->buckets != nullptr, "old level descriptor was overwritten");
      for (uint64_t key = 1; key <= 16; ++key) {
        auto found = table->search(key);
        require(found.found && found.value->second == key + 100, "growth lost KV");
      }
    }
    // No foreground collection calls: the background thread must do all free.
    wait_for([&] { return ub_arena_live(true) == pinned_bytes - retired_bytes; },
             "background did not free exactly the old level and buckets");
    require(ub_arena_live(false) == expected_cache_bytes, "unexpected CACHE allocation or KV reclamation");
  }
  reading.store(false); reader.join();
  require(errors.load() == 0, "concurrent reader lost KV during level reclamation");
  // KV retirement remains intentionally unchanged, even after GC has run.
  auto held = table->search(1);
  table->update({1, 999}, 0);
  table->erase(1, 0);
  usleep(30000);
  require(held.value->second == 101 && ub_arena_live(false) == expected_cache_bytes + 64,
          "level-only GC unexpectedly reclaimed KV objects");
  table->~Table(); shared_atomic_memory().free(table);
}
static void cross_process(char* data, char* nc) {
  auto* table = make_table(data, nc, 2 * 1024 * 1024);
  table->stop_level_gc(); // fork only after both process-local workers stop
  int commands[2], replies[2];
  require(pipe(commands) == 0 && pipe(replies) == 0, "pipe failed");
  const auto child = fork();
  require(child >= 0, "fork failed");
  if (child == 0) {
    close(commands[1]); close(replies[0]);
    try {
      ub_configure_arena(true, nc + 2 * 1024 * 1024, 2 * 1024 * 1024);
      ub_configure_arena(false, data + 32 * 1024, 32 * 1024);
      SimThreadInfo::setup_worker_ids(1, 0);
      auto gc = table->start_level_gc();
      char command;
      {
        ClevelLevelEpochGuard guard(table, table->level_epoch, table->worker_level_epoch(0));
        auto* old = root(table)->last_level;
        const auto old_capacity = old->capacity;
        command = 'H'; require(write(replies[1], &command, 1) == 1, "hold reply failed");
        require(read(commands[0], &command, 1) == 1 && command == 'R', "release command failed");
        require(old->capacity == old_capacity && old->buckets != nullptr, "remote reader's level was reused");
      }
      command = 'R'; require(write(replies[1], &command, 1) == 1, "release reply failed");
      require(read(commands[0], &command, 1) == 1 && command == 'E', "expand command failed");
      expand(table);
      command = 'E'; require(write(replies[1], &command, 1) == 1, "expand reply failed");
      require(read(commands[0], &command, 1) == 1 && command == 'X', "exit command failed");
      // The child's level/buckets have now been retired by the parent resizer.
      // Only the child's published root metadata is deliberately retained.
      const size_t metadata_bytes = (sizeof(Table::level_meta) + 63) & ~size_t(63);
#ifdef OPT_CLEVEL_ROOT_READ
      wait_for([&] { return ub_arena_live(true) == 0; }, "child-owned old level was not reclaimed");
      require(ub_arena_live(false) == metadata_bytes, "child CACHE metadata was not retained");
#else
      wait_for([&] { return ub_arena_live(true) == metadata_bytes; }, "child-owned old level was not reclaimed");
#endif
      gc.reset();
      _exit(0);
    } catch (const std::exception& e) { std::cerr << "child: " << e.what() << std::endl; _exit(1); }
  }
  close(commands[0]); close(replies[1]);
  auto receive = [&](char expected) {
    char reply; require(read(replies[0], &reply, 1) == 1 && reply == expected, "child reply failed");
  };
  auto send = [&](char command) { require(write(commands[1], &command, 1) == 1, "parent command failed"); };
  table->level_gc_worker = table->start_level_gc();
  receive('H');
  size_t freed;
  {
    ClevelLevelEpochGuard guard(table, table->level_epoch, table->worker_level_epoch(0));
    auto* old = root(table)->last_level;
    freed = old->capacity * sizeof(Table::bucket) + ((sizeof(Table::level_bucket) + 63) & ~size_t(63));
  }
  expand(table); migrate(table);
  const auto pinned = ub_arena_live(true);
  usleep(30000);
  require(ub_arena_live(true) == pinned, "remote worker epoch was ignored");
  send('R'); receive('R');
  wait_for([&] { return ub_arena_live(true) == pinned - freed; }, "remote grace period did not finish");
  send('E'); receive('E'); migrate(table);
  for (int i = 0; i < 2; ++i) { expand(table); migrate(table); }
  send('X');
  int status;
  require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child GC failed");
  close(commands[1]); close(replies[0]);
  table->~Table(); shared_atomic_memory().free(table);
}
int main() {
  const size_t bytes = 4ul << 20;
  const std::string name = "clevel-level-gc-" + std::to_string(getpid());
  create_ub_memory(name.c_str(), bytes, "fixture", UINT32_MAX, UbMappingMode::Cacheable);
  create_ub_memory((name + "-nc").c_str(), bytes, "fixture", UINT32_MAX, UbMappingMode::Noncacheable);
  auto* data = static_cast<char*>(map_ub_memory(name.c_str(), nullptr, bytes, UbMappingMode::Cacheable));
  auto* nc = static_cast<char*>(map_ub_memory((name + "-nc").c_str(), nullptr, bytes, UbMappingMode::Noncacheable));
  repeated_growth(data, nc);
  cross_process(data, nc);
  unmap_ub_memory(data, bytes); unmap_ub_memory(nc, bytes);
  destroy_ub_memory(name.c_str()); destroy_ub_memory((name + "-nc").c_str());
  std::cout << "PASS background level-only GC, worker epochs, repeated growth and remote ownership\n";
}
