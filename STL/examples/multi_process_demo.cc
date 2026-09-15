#include "pcc/btree.h"
#include "pcc/init.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

const char *env_or(const char *key, const char *fallback) {
  const char *v = std::getenv(key);
  return (v && v[0]) ? v : fallback;
}

pcc::ShmConfig make_default_config(bool is_creator) {
  pcc::ShmConfig config;
  config.shm_path = env_or("PCC_SHM_PATH", "/dev/shm/cxl_demo");
  config.shm_id = "multi_proc_demo";
  config.shm_size = 1024ULL * 1024 * 1024;
  config.thread_num = 1;
  config.is_creator = is_creator;
  config.allocator_backend = env_or("PCC_ALLOCATOR_BACKEND", "memkind");
  config.cxlalloc_thread_count = 64;
  return config;
}

void process_standalone_attacher() {
  std::cout << "[Attacher Process " << getpid() << "] Starting (standalone)..."
            << std::endl;
  sleep(1);

  pcc::ShmConfig config = make_default_config(false);
  if (!pcc::init_shm(config)) {
    std::cerr << "Failed to initialize shared memory" << std::endl;
    return;
  }

  std::string tree_id = pcc::get_shm_id("bwtree", 789);
  pcc::btree *tree = pcc::btree::init(tree_id, false, 1);
  if (!tree) {
    std::cerr << "Failed to attach to BwTree" << std::endl;
    pcc::cleanup_shm();
    return;
  }

  tree->thread_init(0);
  sleep(1);

  for (int64_t i = 1; i <= 5; ++i) {
    int64_t value;
    if (tree->find(i, value)) {
      std::cout << "[Attacher] Key " << i << " -> Value " << value << std::endl;
    }
  }
  for (int64_t i = 11; i <= 15; ++i) {
    tree->insert(i, i * 10);
  }
  delete tree;
  pcc::cleanup_shm();
}

void run_forked_demo() {
  pcc::ShmConfig cfg = make_default_config(true);
  if (!pcc::init_shm(cfg)) {
    std::cerr << "Failed to initialize shared memory" << std::endl;
    return;
  }

  const std::string tree_id = pcc::get_shm_id("bwtree", 789);
  pcc::btree *creator_tree = pcc::btree::init(tree_id, true, 1);
  if (!creator_tree) {
    std::cerr << "Failed to create BwTree" << std::endl;
    pcc::cleanup_shm();
    return;
  }
  creator_tree->thread_init(0);

  std::cout << "[Creator " << getpid() << "] Inserting keys 1..5" << std::endl;
  for (int64_t i = 1; i <= 5; ++i) {
    creator_tree->insert(i, i * 10);
  }

  const pid_t pid = fork();
  if (pid < 0) {
    std::cerr << "fork failed" << std::endl;
    delete creator_tree;
    pcc::cleanup_shm();
    return;
  }

  if (pid == 0) {
    // memkind is not reliably fork-safe for concurrent allocators; keep the
    // child read-only (no insert / no extra malloc) after fork().
    std::cout << "[Attacher " << getpid() << "] (inherited mmap, read-only)"
              << std::endl;
    pcc::btree *t = pcc::btree::init(tree_id, false, 1);
    if (!t) {
      std::cerr << "child attach failed" << std::endl;
      _exit(1);
    }
    t->thread_init(0);
    for (int64_t i = 1; i <= 5; ++i) {
      int64_t value = 0;
      if (t->find(i, value)) {
        std::cout << "[Attacher] Key " << i << " -> " << value << std::endl;
      }
    }
    delete t;
    _exit(0);
  }

  if (waitpid(pid, nullptr, 0) < 0) {
    std::cerr << "waitpid failed" << std::endl;
  }

  std::cout << "[Creator " << getpid() << "] Inserting keys 6..15" << std::endl;
  for (int64_t i = 6; i <= 15; ++i) {
    creator_tree->insert(i, i * 10);
  }

  std::cout << "[Creator " << getpid() << "] Reading keys 1..15" << std::endl;
  for (int64_t i = 1; i <= 15; ++i) {
    int64_t value;
    if (creator_tree->find(i, value)) {
      std::cout << "[Creator] Key " << i << " -> " << value << std::endl;
    }
  }

  delete creator_tree;
  pcc::cleanup_shm();
  std::cout << "[Creator] Done" << std::endl;
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc > 1 && std::string(argv[1]) == "attacher") {
    process_standalone_attacher();
    return 0;
  }
  run_forked_demo();
  return 0;
}
