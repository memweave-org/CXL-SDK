#pragma once
#include <atomic>

#define DEFAULT_MACHINE_COUNT (16)

class SimThreadInfo {
public:
  enum class THREAD_ROLE {
    DISPATCHER = 1,
    WORKER = 2,
  };

  enum class PROCESS_ROLE {
    MASTER = 1,
    FOLLOWER = 2,
  };

  static thread_local THREAD_ROLE thread_role;
  static PROCESS_ROLE process_role;

  static thread_local uint32_t worker_machine_id;
  static uint32_t worker_machine_count;

  static thread_local uint32_t dispatcher_thread_id;
  static uint32_t dispatcher_thread_count;

  static thread_local uint32_t worker_thread_id;
  static uint32_t worker_thread_count;
  // static std::atomic<int> worker_thread_allocator;

  static thread_local uint32_t worker_db_id;
  static uint32_t worker_db_count;
  static std::atomic<uint32_t> worker_db_id_allocator;

  static int machine_id;

  static void setup_worker_machine_count(int count) {
    worker_machine_count = count;
  }

  static void setup_worker_db_id() {
    worker_db_id = worker_db_id_allocator.fetch_add(1);
    worker_machine_id = worker_db_id % worker_machine_count;
  }

  static void setup_worker_ids(int mid, int thread_id) {
    thread_role = THREAD_ROLE::WORKER;
    worker_machine_id = mid;
    worker_thread_id = thread_id;
  }

  static void setup_dispatcher_id(int thread_id) {
    thread_role = THREAD_ROLE::DISPATCHER;
    dispatcher_thread_id = thread_id;
  }

  static void setup_dispatcher_nr(int nr) { dispatcher_thread_count = nr; }

  static void setup_worker_nr(int nr) { worker_thread_count = nr; }

  static void setup_worker_db_count(int count) { worker_db_count = count; }

  static void setup_machine_ids(int id) {
    worker_machine_id = id;
    process_role =
        worker_machine_id == 0 ? PROCESS_ROLE::MASTER : PROCESS_ROLE::FOLLOWER;
  }

  static bool is_master() { return process_role == PROCESS_ROLE::MASTER; }
  static bool is_follower() { return process_role == PROCESS_ROLE::FOLLOWER; }
};
