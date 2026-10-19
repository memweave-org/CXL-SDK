#include "Tree.h"

#include <city.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <queue>
#include <unistd.h>
#include <utility>
#include <vector>
#include <memory>

#include "Common.h"
#include "GlobalAddress.h"
#include "IndexCache.h"
#include "RdmaBuffer.h"
#include "Timer.h"
#include "utils/sim_id.h"
#include "GlobalLock.h"

bool enter_debug = false;

uint64_t cache_miss[MAX_APP_THREAD][8];
uint64_t cache_hit[MAX_APP_THREAD][8];
uint64_t latency[MAX_APP_THREAD][LATENCY_WINDOWS];

thread_local CoroCall Tree::worker[define::kMaxCoro];
thread_local CoroCall Tree::master;
thread_local GlobalAddress path_stack[define::kMaxCoro]
                                     [define::kMaxLevelOfTree];

thread_local ShermanTimer timer;
thread_local std::queue<uint16_t> hot_wait_queue;
thread_local IndexCache *index_cache;
thread_local LocalLockNode *local_locks;

nt<void *> g_root_ptr{nullptr};
nt<int> g_root_level{-1};
// thread_local void *local_root_ptr = nullptr;
// thread_local int local_root_level = -1;
// thread_local std::atomic<bool> enable_cache{false};
void *local_root_ptr = nullptr;
int local_root_level = -1;
std::atomic<bool> enable_cache{false};

Tree::Tree(DSM *dsm, uint16_t tree_id) : dsm(dsm), tree_id(tree_id) {
  init_global_lock_table();
  init_root_ptr();
  for (int i = 0; i < dsm->getClusterSize(); ++i) {
    g_local_locks[i] = new LocalLockNode[define::kNumOfLock];
    for (size_t k = 0; k < define::kNumOfLock; ++k) {
      auto &n = g_local_locks[i][k];
      n.ticket_lock.store(0);
      n.hand_over = false;
      n.hand_time = 0;
    }
  }

  assert(dsm->is_register());
  print_verbose();

  for (int i = 0; i < dsm->getClusterSize(); i++) {
    g_index_cache[i] = new IndexCache(define::kIndexCacheSize);
  }

  root_ptr_ptr = &root_ptr_list[tree_id];

  // try to init tree and install root pointer
  auto page_buffer = (dsm->get_rbuf(0)).get_page_buffer();
  auto root_addr = dsm->alloc(kLeafPageSize);
  auto root_page = new (page_buffer) LeafPage;

  root_page->set_consistent();
  dsm->write_sync(page_buffer, root_addr, kLeafPageSize);

  void* expected = nullptr;
  bool res =
      root_ptr_ptr->compare_exchange_strong(expected, (void *)root_addr.val);
  if (res) {
    std::cout << "Tree root pointer value " << root_addr << std::endl;
  } else {
    // std::cout << "fail\n";
  }
  g_root_ptr = (void *)root_addr.val;
}

void Tree::print_verbose() {
  int kLeafHdrOffset = STRUCT_OFFSET(LeafPage, hdr);
  int kInternalHdrOffset = STRUCT_OFFSET(InternalPage, hdr);
  if (kLeafHdrOffset != kInternalHdrOffset) {
    std::cerr << "format error" << std::endl;
  }

  if (dsm->getMyNodeID() == 0) {
    std::cout << "Header size: " << sizeof(Header) << std::endl;
    std::cout << "Internal Page size: " << sizeof(InternalPage) << " ["
              << kInternalPageSize << "]" << std::endl;
    std::cout << "Internal per Page: " << kInternalCardinality << std::endl;
    std::cout << "Leaf Page size: " << sizeof(LeafPage) << " [" << kLeafPageSize
              << "]" << std::endl;
    std::cout << "Leaf per Page: " << kLeafCardinality << std::endl;
    std::cout << "LeafEntry size: " << sizeof(LeafEntry) << std::endl;
    std::cout << "InternalEntry size: " << sizeof(InternalEntry) << std::endl;
  }
}

inline void Tree::before_operation(CoroContext *cxt, int coro_id) {
  for (size_t i = 0; i < define::kMaxLevelOfTree; ++i) {
    path_stack[coro_id][i] = GlobalAddress::Null();
  }
}

GlobalAddress Tree::get_root_ptr(CoroContext *cxt, int coro_id) {
  // if (local_root_ptr != g_root_ptr) {
  //   local_root_ptr = g_root_ptr;
  //   local_root_level = g_root_level;
  //   if (local_root_level >= 3) {
  //     enable_cache = true;
  //   }
  // }
  // GlobalAddress addr;
  // addr.val = (uint64_t)local_root_ptr;
  GlobalAddress addr;
  addr.val = (uint64_t)(root_ptr_ptr->load());
  return addr;
  // std::cout << "root ptr " << root_ptr << std::endl;
}

bool Tree::update_new_root(GlobalAddress left, const ShermanKey &k,
                           GlobalAddress right, int level,
                           GlobalAddress old_root, CoroContext *cxt,
                           int coro_id) {
  auto page_buffer = dsm->get_rbuf(coro_id).get_page_buffer();
  auto new_root = new (page_buffer) InternalPage(left, k, right, level);

  auto new_root_addr = dsm->alloc(kInternalPageSize);

  new_root->set_consistent();
  dsm->write_sync(page_buffer, new_root_addr, kInternalPageSize, cxt);
  void* expected = (void *)old_root.val;
  if (root_ptr_ptr->compare_exchange_strong(expected,
                                            (void *)new_root_addr.val)) {
    g_root_ptr = (void *)new_root_addr.val;
    g_root_level = level;
    // local_root_ptr = (void *)new_root_addr.val;
    // local_root_level = level;
    if (level >= 3) {
      enable_cache = true;
    }
    std::cout << "new root level " << level << " " << new_root_addr
              << std::endl;
    return true;
  } else {
    std::cout << "cas root fail " << std::endl;
  }

  return false;
}

void Tree::print_and_check_tree(CoroContext *cxt, int coro_id) {
  assert(dsm->is_register());

  auto root = get_root_ptr(cxt, coro_id);
  // SearchResult result;

  GlobalAddress p = root;
  GlobalAddress levels[define::kMaxLevelOfTree];
  int level_cnt = 0;
  auto page_buffer = (dsm->get_rbuf(coro_id)).get_page_buffer();
  GlobalAddress leaf_head;

next_level:

  dsm->read_sync(page_buffer, p, kLeafPageSize);
  auto header = (Header *)(page_buffer + (STRUCT_OFFSET(LeafPage, hdr)));
  levels[level_cnt++] = p;
  if (header->level != 0) {
    p = header->leftmost_ptr;
    goto next_level;
  } else {
    leaf_head = p;
  }

next:
  dsm->read_sync(page_buffer, leaf_head, kLeafPageSize);
  auto page = (LeafPage *)page_buffer;
  for (int i = 0; i < kLeafCardinality; ++i) {
    if (page->records[i].value != kValueNull) {
    }
  }
  while (page->hdr.sibling_ptr != GlobalAddress::Null()) {
    leaf_head = page->hdr.sibling_ptr;
    goto next;
  }

  // for (int i = 0; i < level_cnt; ++i) {
  //   dsm->read_sync(page_buffer, levels[i], kLeafPageSize);
  //   auto header = (Header *)(page_buffer + (STRUCT_OFFSET(LeafPage, hdr)));
  //   // std::cout << "addr: " << levels[i] << " ";
  //   // header->debug();
  //   // std::cout << " | ";
  //   while (header->sibling_ptr != GlobalAddress::Null()) {
  //     dsm->read_sync(page_buffer, header->sibling_ptr, kLeafPageSize);
  //     header = (Header *)(page_buffer + (STRUCT_OFFSET(LeafPage, hdr)));
  //     // std::cout << "addr: " << header->sibling_ptr << " ";
  //     // header->debug();
  //     // std::cout << " | ";
  //   }
  //   // std::cout << "\n------------------------------------" << std::endl;
  //   // std::cout << "------------------------------------" << std::endl;
  // }
}

inline bool Tree::try_lock_addr(GlobalAddress lock_addr, uint64_t tag,
                                uint64_t *buf, CoroContext *cxt, int coro_id) {
  bool hand_over = acquire_local_lock(lock_addr, cxt, coro_id);
  if (hand_over) {
    return true;
  }

  {
    uint64_t retry_cnt = 0;
    uint64_t pre_tag = 0;
    uint64_t conflict_tag = 0;
  retry:
    retry_cnt++;
    if (retry_cnt > 1000000) {
      std::cout << "Deadlock " << lock_addr << std::endl;

      std::cout << dsm->getMyNodeID() << ", " << dsm->getMyThreadID()
                << " locked by " << (conflict_tag >> 32) << ", "
                << (conflict_tag << 32 >> 32) << std::endl;
      assert(false);
    }

    nt<uint64_t> *lock = (nt<uint64_t> *)lock_addr.val;
    uint64_t expected = 0;
    bool res = lock->compare_exchange_strong(expected, tag);
    // bool res = dsm->cas_dm_sync(lock_addr, 0, tag, buf, cxt);

    if (!res) {
      // std::cout << "lock failed" << dsm->getMyThreadID() << " " << std::hex
      //           << lock_addr.val << std::dec << " " << *(uint64_t *)lock_addr.val
      //           << std::endl;
      conflict_tag = expected - 1;
      if (conflict_tag != pre_tag) {
        retry_cnt = 0;
        pre_tag = conflict_tag;
      }
      goto retry;
    }
  }
  // sleep(1);
  // std::cout << "lock " << dsm->getMyThreadID() << " " << std::hex
  //           << lock_addr.val << std::dec << " " << *(uint64_t*)lock_addr.val << std::endl;

  return true;
}

inline void Tree::unlock_addr(GlobalAddress lock_addr, uint64_t tag,
                              uint64_t *buf, CoroContext *cxt, int coro_id,
                              bool async) {
  bool hand_over_other = can_hand_over(lock_addr);
  if (hand_over_other) {
    release_local_lock(lock_addr);
    return;
  }

  // auto cas_buf = dsm->get_rbuf(coro_id).get_cas_buffer();
  // *cas_buf = 0;

  nt<uint64_t> *lock = (nt<uint64_t> *)lock_addr.val;
  lock->store(0);

  // if (async) {
  //   dsm->write_dm((char *)cas_buf, lock_addr, sizeof(uint64_t), false);
  // } else {
  //   dsm->write_dm_sync((char *)cas_buf, lock_addr, sizeof(uint64_t), cxt);
  // }

  release_local_lock(lock_addr);
  // std::cout << "unlock " << dsm->getMyThreadID() << " " << std::hex
  //           << lock_addr.val << std::dec << std::endl;
}

void Tree::write_page_and_unlock(char *page_buffer, GlobalAddress page_addr,
                                 int page_size, uint64_t *cas_buffer,
                                 GlobalAddress lock_addr, uint64_t tag,
                                 CoroContext *cxt, int coro_id, bool async) {
  bool hand_over_other = can_hand_over(lock_addr);
  if (hand_over_other) {
    dsm->write_sync(page_buffer, page_addr, page_size, cxt);
    release_local_lock(lock_addr);
    return;
  }

  DSM::CXLOpRegion rs[1];
  rs[0].source = (uint64_t)page_buffer;
  rs[0].dest = page_addr.val;
  rs[0].size = page_size;

  // rs[1].source = (uint64_t)dsm->get_rbuf(coro_id).get_cas_buffer();
  // rs[1].dest = lock_addr.val;
  // rs[1].size = sizeof(uint64_t);

  // *(uint64_t *)rs[1].source = 0;
  if (async) {
    dsm->write_batch(rs, 1, false);
  } else {
    dsm->write_batch_sync(rs, 1, cxt);
  }

  // unlock_addr(lock_addr, dsm->getThreadTag(), nullptr, nullptr, 0, true);

  // release_local_lock(lock_addr);
  nt<uint64_t> *lock = (nt<uint64_t> *)lock_addr.val;
  lock->store(0);

  release_local_lock(lock_addr);
    // std::cout << "unlock " << dsm->getMyThreadID() << " " << std::hex
    //         << lock_addr.val << std::dec << std::endl;
}

void Tree::lock_and_read_page(char *page_buffer, GlobalAddress page_addr,
                              int page_size, uint64_t *cas_buffer,
                              GlobalAddress lock_addr, uint64_t tag,
                              CoroContext *cxt, int coro_id) {
  try_lock_addr(lock_addr, tag, cas_buffer, cxt, coro_id);

  dsm->read_sync(page_buffer, page_addr, page_size, cxt);
}

void Tree::lock_bench(const ShermanKey &k, CoroContext *cxt, int coro_id) {
  uint64_t lock_index = CityHash64((char *)&k, sizeof(k)) % define::kNumOfLock;

  GlobalAddress lock_addr;
  // lock_addr.nodeID = 0;
  // lock_addr.offset = lock_index * sizeof(uint64_t);
  lock_addr.val = (uint64_t)&global_lock_table[lock_index];
  auto cas_buffer = dsm->get_rbuf(coro_id).get_cas_buffer();

  // bool res = dsm->cas_sync(lock_addr, 0, 1, cas_buffer, cxt);
  try_lock_addr(lock_addr, 1, cas_buffer, cxt, coro_id);
  unlock_addr(lock_addr, 1, cas_buffer, cxt, coro_id, true);
}

void Tree::insert_internal(const ShermanKey &k, GlobalAddress v,
                           CoroContext *cxt, int coro_id, int level) {
restart:
  auto root = get_root_ptr(cxt, coro_id);
  SearchResult result;

  GlobalAddress p = root;

next:
    if (p == GlobalAddress::Null()) {
    goto restart;
  }

  if (!page_search(p, k, result, cxt, coro_id)) {
    std::cout << "SEARCH WARNING insert" << std::endl;
    p = get_root_ptr(cxt, coro_id);
    sleep(1);
    goto next;
  }

  assert(result.level != 0);
  if (result.slibing != GlobalAddress::Null()) {
    p = result.slibing;
    goto next;
  }

  // A root split may have installed the target level as the new root. Store
  // in that page directly; looking only for a parent one level higher would
  // descend past the insertion level.
  if (result.level == level) {
    internal_page_store(p, k, v, root, level, cxt, coro_id);
    return;
  }
  if (result.level < level) goto restart;

  p = result.next_level;
  if (result.level != level + 1) {
    goto next;
  }
      if (p == GlobalAddress::Null()) {
    goto restart;
  }

  internal_page_store(p, k, v, root, level, cxt, coro_id);
}

void Tree::insert(const ShermanKey &k, const Value &v, CoroContext *cxt,
                  int coro_id) {
  ThreadStatus::Guard epoch(index_cache->thread_status,dsm->getMyThreadID());
  assert(dsm->is_register());
restart:
  before_operation(cxt, coro_id);

  if (enable_cache) {
    GlobalAddress cache_addr;
    auto entry = index_cache->search_from_cache(k, &cache_addr,
                                                dsm->getMyThreadID() == 0);
    if(cache_addr == GlobalAddress::Null()) {
      goto disable_cache;
    }
    if (entry) {  // cache hit
      auto root = get_root_ptr(cxt, coro_id);
      if (leaf_page_store(cache_addr, k, v, root, 0, cxt, coro_id, true)) {
        cache_hit[dsm->getMyThreadID()][0]++;
        return;
      }
      // cache stale, from root,
      index_cache->invalidate(entry);
    }
    cache_miss[dsm->getMyThreadID()][0]++;
  }

disable_cache:
  auto root = get_root_ptr(cxt, coro_id);
  SearchResult result;

  GlobalAddress p = root;

next:
  if (p == GlobalAddress::Null()) {
    goto restart;
  }

  if (!page_search(p, k, result, cxt, coro_id)) {
    std::cout << "SEARCH WARNING insert" << std::endl;
    p = get_root_ptr(cxt, coro_id);
    sleep(1);
    goto next;
  }

  if (!result.is_leaf) {
    assert(result.level != 0);
    if (result.slibing != GlobalAddress::Null()) {
      p = result.slibing;
      goto next;
    }

    p = result.next_level;
    if (result.level != 1) {
      goto next;
    }
  }

  if (p == GlobalAddress::Null()) {
    goto restart;
  }

  // A root change or stale route aborts the attempt without publishing k.
  // Retry instead of reporting a successful insertion to the benchmark.
  if (!leaf_page_store(p, k, v, root, 0, cxt, coro_id)) goto restart;

}

bool Tree::search(const ShermanKey &k, Value &v, CoroContext *cxt,
                  int coro_id) {
  ThreadStatus::Guard epoch(index_cache->thread_status,dsm->getMyThreadID());
  assert(dsm->is_register());

  auto root = get_root_ptr(cxt, coro_id);
  SearchResult result;

  GlobalAddress p = root;

  bool from_cache = false;
  const CacheEntry *entry = nullptr;
  if (enable_cache) {
    GlobalAddress cache_addr;
    entry = index_cache->search_from_cache(k, &cache_addr,
                                           dsm->getMyThreadID() == 0);
    if (entry) {  // cache hit
      cache_hit[dsm->getMyThreadID()][0]++;
      from_cache = true;
      p = cache_addr;

    } else {
      cache_miss[dsm->getMyThreadID()][0]++;
    }
  }

next:
  if (!page_search(p, k, result, cxt, coro_id, from_cache)) {
    if (from_cache) {  // cache stale
      index_cache->invalidate(entry);
      cache_hit[dsm->getMyThreadID()][0]--;
      cache_miss[dsm->getMyThreadID()][0]++;
      from_cache = false;

      p = root;
    } else {
      std::cout << "SEARCH WARNING search" << std::endl;
      sleep(1);
    }
    goto next;
  }
  if (result.is_leaf) {
    if (result.val != kValueNull) {  // find
      v = result.val;
      return true;
    }
    if (result.slibing != GlobalAddress::Null()) {  // turn right
      p = result.slibing;
      goto next;
    }
    return false;  // not found
  } else {         // internal
    p = result.slibing != GlobalAddress::Null() ? result.slibing
                                                : result.next_level;
    goto next;
  }
}

uint64_t Tree::range_query(const ShermanKey &from, const ShermanKey &to,
                           Value *value_buffer, CoroContext *cxt, int coro_id) {
  thread_local std::vector<InternalPage *> result;
  thread_local std::vector<GlobalAddress> leaves;

  result.clear();
  leaves.clear();
  index_cache->search_range_from_cache(from, to, result);

  // FIXME: here, we assume all innernal nodes are cached in compute node
  if (result.empty()) {
    return 0;
  }

  uint64_t counter = 0;
  for (auto page : result) {
    auto cnt = page->hdr.last_index + 1;
    auto addr = page->hdr.leftmost_ptr;

    // [from, to]
    // [lowest, page->records[0].key);
    bool no_fetch = from > page->records[0].key || to < page->hdr.lowest;
    if (!no_fetch) {
      leaves.push_back(addr);
    }
    for (int i = 1; i < cnt; ++i) {
      no_fetch = from > page->records[i].key || to < page->records[i - 1].key;
      if (!no_fetch) {
        leaves.push_back(page->records[i - 1].ptr);
      }
    }

    no_fetch = from > page->hdr.highest || to < page->records[cnt - 1].key;
    if (!no_fetch) {
      leaves.push_back(page->records[cnt - 1].ptr);
    }
  }

  for (size_t i = 0; i < leaves.size(); ++i) {
    LeafPage page;
    dsm->read((char *)&page, leaves[i], kLeafPageSize, true);
    for (int i = 0; i < kLeafCardinality; ++i) {
      auto &r = page.records[i];
      if (r.value != kValueNull && r.f_version == r.r_version) {
        if (r.key >= from && r.key <= to) {
          value_buffer[counter++] = r.value;
        }
      }
    }
  }

  return counter;
}

void Tree::del(const ShermanKey &k, CoroContext *cxt, int coro_id) {
  assert(dsm->is_register());

  before_operation(cxt, coro_id);

  if (enable_cache) {
    GlobalAddress cache_addr;
    auto entry = index_cache->search_from_cache(k, &cache_addr,
                                                dsm->getMyThreadID() == 0);
    if (entry) {  // cache hit
      if (leaf_page_del(cache_addr, k, 0, cxt, coro_id, true)) {
        cache_hit[dsm->getMyThreadID()][0]++;
        return;
      }
      // cache stale, from root,
      index_cache->invalidate(entry);
    }
    cache_miss[dsm->getMyThreadID()][0]++;
  }

  auto root = get_root_ptr(cxt, coro_id);
  SearchResult result;

  GlobalAddress p = root;

next:

  if (!page_search(p, k, result, cxt, coro_id)) {
    std::cout << "SEARCH WARNING del" << std::endl;
    p = get_root_ptr(cxt, coro_id);
    sleep(1);
    goto next;
  }

  if (!result.is_leaf) {
    assert(result.level != 0);
    if (result.slibing != GlobalAddress::Null()) {
      p = result.slibing;
      goto next;
    }

    p = result.next_level;
    if (result.level != 1) {
      goto next;
    }
  }

  leaf_page_del(p, k, 0, cxt, coro_id);
}

bool Tree::page_search(GlobalAddress page_addr, const ShermanKey &k,
                       SearchResult &result, CoroContext *cxt, int coro_id,
                       bool from_cache) {
  auto page_buffer = (dsm->get_rbuf(coro_id)).get_page_buffer();
  auto header = (Header *)(page_buffer + (STRUCT_OFFSET(LeafPage, hdr)));

  int counter = 0;
re_read:
  if (++counter > 100) {
    printf("re read too many times\n");
    sleep(1);
  }
#if defined(SHERMAN_UC_DIRECT) || defined(SHERMAN_UC_ENTRY_COPY)
  // Validate the entire projection, not each small copy separately. No pointer
  // read from the remote node is followed until this snapshot is accepted.
  {
    struct Projection {
      Header header;
      ShermanKey key;
      Value value;
      GlobalAddress next;
      bool consistent;
    } out{};
    out.key = k;
    cxl_transport::read_projected(reinterpret_cast<const void*>(page_addr.val),
        kLeafPageSize, &out, [](const void* source, void* destination) {
      const auto* p = static_cast<const LeafPage*>(source);
      auto& o = *static_cast<Projection*>(destination);
      memcpy(&o.header, &p->hdr, sizeof(Header));
      o.value = kValueNull;
      o.next = GlobalAddress::Null();
      const bool leaf = o.header.leftmost_ptr == GlobalAddress::Null();
      o.consistent = leaf ? p->front_version == p->rear_version :
          static_cast<const InternalPage*>(source)->front_version ==
          static_cast<const InternalPage*>(source)->rear_version;
      if (!o.consistent || o.key < o.header.lowest || o.key >= o.header.highest) return;
      if (leaf) {
        for (int i = 0; i < kLeafCardinality; ++i) {
#ifdef SHERMAN_UC_ENTRY_COPY
          LeafEntry local;
          // Force the requested entry-sized libc copy, not scalar projection.
          void* (*volatile copy)(void*, const void*, size_t) = &memcpy;
          copy(&local, &p->records[i], sizeof(local));
          const auto& entry = local;
#else
          const auto& entry = p->records[i];
#endif
          if (entry.key == o.key && entry.value != kValueNull &&
              entry.f_version == entry.r_version) {
            o.value = entry.value;
            break;
          }
        }
      } else {
        const auto* internal = static_cast<const InternalPage*>(source);
        // A transient/corrupt count must never become an out-of-bounds read,
        // even before the enclosing sequence validation rejects the snapshot.
        if (o.header.last_index < 0 || o.header.last_index >= kInternalCardinality) {
          o.consistent = false;
          return;
        }
        o.next = o.header.leftmost_ptr;
        for (int i = 0; i <= o.header.last_index; ++i) {
#ifdef SHERMAN_UC_ENTRY_COPY
          InternalEntry local;
          void* (*volatile copy)(void*, const void*, size_t) = &memcpy;
          copy(&local, &internal->records[i], sizeof(local));
          const auto& entry = local;
#else
          const auto& entry = internal->records[i];
#endif
          if (o.key < entry.key) break;
          o.next = entry.ptr;
        }
      }
    });
    if (!out.consistent) goto re_read;
    if (from_cache && (k < out.header.lowest || k >= out.header.highest)) return false;
    if (k < out.header.lowest) return false;
    // Retain the same local index cache. Refilling a level-1 cache entry needs
    // a complete validated internal-node snapshot in both variants.
    if (out.header.leftmost_ptr != GlobalAddress::Null() &&
        out.header.level == 1 && enable_cache) goto full_page_read;
    memset(&result, 0, sizeof(result));
    result.is_leaf = out.header.leftmost_ptr == GlobalAddress::Null();
    result.level = out.header.level;
    path_stack[coro_id][result.level] = page_addr;
    result.val = out.value;
    result.next_level = out.next;
    if (k >= out.header.highest) result.slibing = out.header.sibling_ptr;
    return true;
  }
full_page_read:
#endif
#ifdef SHERMAN_SELECTIVE
  // Probe the header, then project leaves in one validated snapshot. Internal
  // pages retain the full-page path and index cache.
  {
    struct Projection { Header header; ShermanKey key; Value value; bool consistent; };
    Projection out{};
    out.key = k;
    cxl_transport::read_projected(reinterpret_cast<const void*>(page_addr.val),
        kLeafPageSize, &out, [](const void* source, void* destination) {
      auto* p = static_cast<const LeafPage*>(source);
      auto& o = *static_cast<Projection*>(destination);
      memcpy(&o.header, &p->hdr, sizeof(Header));
      o.value = kValueNull;
      o.consistent = p->front_version == p->rear_version;
      if (o.header.leftmost_ptr != GlobalAddress::Null() ||
          o.key < o.header.lowest || o.key >= o.header.highest) return;
      // Leaves are unsorted: scan only until the matching entry. Source cache
      // lines are fetched on demand; no full-node scratch copy or NT stores.
      for (int i = 0; i < kLeafCardinality; ++i) {
        LeafEntry entry;
        memcpy(&entry, &p->records[i], sizeof(entry));
        if (entry.key == o.key && entry.value != kValueNull &&
            entry.f_version == entry.r_version) {
          o.value = entry.value;
          break;
        }
      }
    });
    if (out.header.leftmost_ptr == GlobalAddress::Null()) {
      if (!out.consistent) goto re_read;
      if (from_cache && (k < out.header.lowest || k >= out.header.highest)) return false;
      if (k < out.header.lowest) return false;
      memset(&result, 0, sizeof(result));
      result.is_leaf = true;
      result.val = out.value;
      if (k >= out.header.highest) result.slibing = out.header.sibling_ptr;
      path_stack[coro_id][0] = page_addr;
      return true;
    }
  }
#endif
  dsm->read_sync(page_buffer, page_addr, kLeafPageSize, cxt);

  memset(&result, 0, sizeof(result));
  result.is_leaf = header->leftmost_ptr == GlobalAddress::Null();
  result.level = header->level;
  path_stack[coro_id][result.level] = page_addr;
  // std::cout << "level " << (int)result.level << " " << page_addr <<
  // std::endl;

  if (result.is_leaf) {
    auto page = (LeafPage *)page_buffer;
    if (!page->check_consistent()) {
      goto re_read;
    }

    if (from_cache &&
        (k < page->hdr.lowest || k >= page->hdr.highest)) {  // cache is stale
      return false;
    }

    assert(result.level == 0);
    if (k >= page->hdr.highest) {  // should turn right
      result.slibing = page->hdr.sibling_ptr;
      return true;
    }
    if (k < page->hdr.lowest) {
      assert(false);
      return false;
    }
    leaf_page_search(page, k, result);
  } else {
    assert(result.level != 0);
    assert(!from_cache);
    auto page = (InternalPage *)page_buffer;

    if (!page->check_consistent()) {
      goto re_read;
    }

    if (result.level == 1 && enable_cache) {
      index_cache->add_to_cache(page);
    }

    if (k >= page->hdr.highest) {  // should turn right
      result.slibing = page->hdr.sibling_ptr;
      return true;
    }
    if (k < page->hdr.lowest) {
      printf("key %ld error in level %d\n", k, page->hdr.level);
      sleep(10);
      print_and_check_tree();
      assert(false);
      return false;
    }
    internal_page_search(page, k, result);
  }

  return true;
}

void Tree::internal_page_search(InternalPage *page, const ShermanKey &k,
                                SearchResult &result) {
  assert(k >= page->hdr.lowest);
  assert(k < page->hdr.highest);

  auto cnt = page->hdr.last_index + 1;
  // page->debug();
  if (k < page->records[0].key) {
    result.next_level = page->hdr.leftmost_ptr;
    return;
  }

  for (int i = 1; i < cnt; ++i) {
    if (k < page->records[i].key) {
      result.next_level = page->records[i - 1].ptr;
      return;
    }
  }
  result.next_level = page->records[cnt - 1].ptr;
}

void Tree::leaf_page_search(LeafPage *page, const ShermanKey &k,
                            SearchResult &result) {
  for (int i = 0; i < kLeafCardinality; ++i) {
    auto &r = page->records[i];
    if (r.key == k && r.value != kValueNull && r.f_version == r.r_version) {
      result.val = r.value;
      break;
    }
  }
}

void Tree::internal_page_store(GlobalAddress page_addr, const ShermanKey &k,
                               GlobalAddress v, GlobalAddress root, int level,
                               CoroContext *cxt, int coro_id) {
  uint64_t lock_index =
      CityHash64((char *)&page_addr, sizeof(page_addr)) % define::kNumOfLock;

  GlobalAddress lock_addr;
  // lock_addr.nodeID = page_addr.nodeID;
  // lock_addr.offset = lock_index * sizeof(uint64_t);
  lock_addr.val = (uint64_t)&(global_lock_table[lock_index]);

  auto &rbuf = dsm->get_rbuf(coro_id);
  uint64_t *cas_buffer = rbuf.get_cas_buffer();
  auto page_buffer = rbuf.get_page_buffer();

  auto tag = dsm->getThreadTag();
  assert(tag != 0);

  lock_and_read_page(page_buffer, page_addr, kInternalPageSize, cas_buffer,
                     lock_addr, tag, cxt, coro_id);

  auto page = (InternalPage *)page_buffer;

  assert(page->hdr.level == level);
  assert(page->check_consistent());
  if (k >= page->hdr.highest) {
    this->unlock_addr(lock_addr, tag, cas_buffer, cxt, coro_id, true);

    assert(page->hdr.sibling_ptr != GlobalAddress::Null());

    this->internal_page_store(page->hdr.sibling_ptr, k, v, root, level, cxt,
                              coro_id);

    return;
  }
  assert(k >= page->hdr.lowest);

  auto cnt = page->hdr.last_index + 1;

  bool is_update = false;
  uint16_t insert_index = 0;
  for (int i = cnt - 1; i >= 0; --i) {
    if (page->records[i].key == k) {  // find and update
      page->records[i].ptr = v;
      // assert(false);
      is_update = true;
      break;
    }
    if (page->records[i].key < k) {
      insert_index = i + 1;
      break;
    }
  }

  assert(cnt != kInternalCardinality);

  if (!is_update) {  // insert and shift
    for (int i = cnt; i > insert_index; --i) {
      page->records[i].key = page->records[i - 1].key;
      page->records[i].ptr = page->records[i - 1].ptr;
    }
    page->records[insert_index].key = k;
    page->records[insert_index].ptr = v;

    page->hdr.last_index++;
  }

  cnt = page->hdr.last_index + 1;
  bool need_split = cnt == kInternalCardinality;
  ShermanKey split_key;
  GlobalAddress sibling_addr;
  if (need_split) {  // need split
    sibling_addr = dsm->alloc(kInternalPageSize);
    auto sibling_buf = rbuf.get_sibling_buffer();

    auto sibling = new (sibling_buf) InternalPage(page->hdr.level);

    //    std::cout << "addr " <<  sibling_addr << " | level " <<
    //    (int)(page->hdr.level) << std::endl;

    int m = cnt / 2;
    split_key = page->records[m].key;
    assert(split_key > page->hdr.lowest);
    assert(split_key < page->hdr.highest);
    for (int i = m + 1; i < cnt; ++i) {  // move
      sibling->records[i - m - 1].key = page->records[i].key;
      sibling->records[i - m - 1].ptr = page->records[i].ptr;
    }
    page->hdr.last_index -= (cnt - m);
    sibling->hdr.last_index += (cnt - m - 1);

    sibling->hdr.leftmost_ptr = page->records[m].ptr;
    sibling->hdr.lowest = page->records[m].key;
    sibling->hdr.highest = page->hdr.highest;
    page->hdr.highest = page->records[m].key;

    // link
    sibling->hdr.sibling_ptr = page->hdr.sibling_ptr;
    page->hdr.sibling_ptr = sibling_addr;

    sibling->set_consistent();
    dsm->write_sync(sibling_buf, sibling_addr, kInternalPageSize, cxt);
  }

  page->set_consistent();
  write_page_and_unlock(page_buffer, page_addr, kInternalPageSize, cas_buffer,
                        lock_addr, tag, cxt, coro_id, need_split);

  if (!need_split) return;

  if (root == page_addr) {  // update root

    if (update_new_root(page_addr, split_key, sibling_addr, level + 1, root,
                        cxt, coro_id)) {
      return;
    }
  }

  auto up_level = path_stack[coro_id][level + 1];

  if (up_level != GlobalAddress::Null()) {
    internal_page_store(up_level, split_key, sibling_addr, root, level + 1, cxt,
                        coro_id);
  } else {
    // Cache-based traversals and concurrent root replacement can leave this
    // path without a recorded parent. The split is already published: locate
    // its parent from the authoritative root instead of losing the separator.
    insert_internal(split_key, sibling_addr, cxt, coro_id, level + 1);
  }
}

bool Tree::leaf_page_store(GlobalAddress page_addr, const ShermanKey &k,
                           const Value &v, GlobalAddress root, int level,
                           CoroContext *cxt, int coro_id, bool from_cache) {
  uint64_t lock_index =
      CityHash64((char *)&page_addr, sizeof(page_addr)) % define::kNumOfLock;

  GlobalAddress lock_addr;

#ifdef CONFIG_ENABLE_EMBEDDING_LOCK
  lock_addr = page_addr;
#else
  // lock_addr.nodeID = page_addr.nodeID;
  // lock_addr.offset = lock_index * sizeof(uint64_t);
  lock_addr.val = (uint64_t)&global_lock_table[lock_index];
#endif

  auto &rbuf = dsm->get_rbuf(coro_id);
  uint64_t *cas_buffer = rbuf.get_cas_buffer();
  auto page_buffer = rbuf.get_page_buffer();

  auto tag = dsm->getThreadTag();
  assert(tag != 0);

  lock_and_read_page(page_buffer, page_addr, kLeafPageSize, cas_buffer,
                     lock_addr, tag, cxt, coro_id);

  auto page = (LeafPage *)page_buffer;

  assert(page->hdr.level == level);
  assert(page->check_consistent());

  if (from_cache &&
      (k < page->hdr.lowest || k >= page->hdr.highest)) {  // cache is stale
    this->unlock_addr(lock_addr, tag, cas_buffer, cxt, coro_id, true);
    return false;
  }

  // if (get_root_ptr(cxt, coro_id) != root) {
  //   this->unlock_addr(lock_addr, tag, cas_buffer, cxt, coro_id, true);
  //   return false;
  // }

  // assert(k < page->hdr.lowest);
  if (k < page->hdr.lowest) {
    this->unlock_addr(lock_addr, tag, cas_buffer, cxt, coro_id, true);
    return false;
  }

  if (k >= page->hdr.highest) {
    this->unlock_addr(lock_addr, tag, cas_buffer, cxt, coro_id, true);
    assert(page->hdr.sibling_ptr != GlobalAddress::Null());
    return this->leaf_page_store(page->hdr.sibling_ptr, k, v, root, level, cxt,
                                 coro_id);
  }
  assert(k >= page->hdr.lowest);

  int cnt = 0;
  int empty_index = -1;
  char *update_addr = nullptr;
  for (int i = 0; i < kLeafCardinality; ++i) {
    auto &r = page->records[i];
    if (r.value != kValueNull) {
      cnt++;
      if (r.key == k) {
        r.value = v;
        r.f_version++;
        r.r_version = r.f_version;
        update_addr = (char *)&r;
        break;
      }
    } else if (empty_index == -1) {
      empty_index = i;
    }
  }

  assert(cnt != kLeafCardinality);

  if (update_addr == nullptr) {  // insert new item
    if (empty_index == -1) {
      printf("%d cnt\n", cnt);
      assert(false);
    }

    auto &r = page->records[empty_index];
    r.key = k;
    r.value = v;
    r.f_version++;
    r.r_version = r.f_version;

    update_addr = (char *)&r;

    cnt++;
  }

  bool need_split = cnt == kLeafCardinality;
  if (!need_split) {
    assert(update_addr);
    write_page_and_unlock(
        update_addr, GADD(page_addr, (update_addr - (char *)page)),
        sizeof(LeafEntry), cas_buffer, lock_addr, tag, cxt, coro_id, false);

    return true;
  } else {
    std::sort(
        page->records, page->records + kLeafCardinality,
        [](const LeafEntry &a, const LeafEntry &b) { return a.key < b.key; });
  }

  ShermanKey split_key;
  GlobalAddress sibling_addr;

  // if (get_root_ptr(cxt, coro_id) != root) {
  //   nt<uint64_t> *lock = (nt<uint64_t> *)lock_addr.val;
  //   lock->store(0);

  //   release_local_lock(lock_addr);
  //   return false;
  // }

  if (need_split) {  // need split
    sibling_addr = dsm->alloc(kLeafPageSize);
    auto sibling_buf = rbuf.get_sibling_buffer();

    auto sibling = new (sibling_buf) LeafPage(page->hdr.level);

    // std::cout << "addr " <<  sibling_addr << " | level " <<
    // (int)(page->hdr.level) << std::endl;

    int m = cnt / 2;
    split_key = page->records[m].key;
    assert(split_key > page->hdr.lowest);
    assert(split_key < page->hdr.highest);

    for (int i = m; i < cnt; ++i) {  // move
      sibling->records[i - m].key = page->records[i].key;
      sibling->records[i - m].value = page->records[i].value;
      page->records[i].key = 0;
      page->records[i].value = kValueNull;
    }
    page->hdr.last_index -= (cnt - m);
    sibling->hdr.last_index += (cnt - m);

    sibling->hdr.lowest = split_key;
    sibling->hdr.highest = page->hdr.highest;
    page->hdr.highest = split_key;

    // link
    sibling->hdr.sibling_ptr = page->hdr.sibling_ptr;
    page->hdr.sibling_ptr = sibling_addr;

    sibling->set_consistent();
    dsm->write_sync(sibling_buf, sibling_addr, kLeafPageSize, cxt);
  }

  page->set_consistent();

  if (!need_split) {
    write_page_and_unlock(page_buffer, page_addr, kLeafPageSize, cas_buffer,
                          lock_addr, tag, cxt, coro_id, false);
    return true;
  }

  if (root == page_addr) { // update root
    if (update_new_root(page_addr, split_key, sibling_addr, level + 1, root,
                        cxt, coro_id)) {
      write_page_and_unlock(page_buffer, page_addr, kLeafPageSize, cas_buffer,
                            lock_addr, tag, cxt, coro_id, need_split);
      return true;
    } else {
      unlock_addr(lock_addr, tag, cas_buffer, cxt, coro_id, true);
      dsm->free(sibling_addr);
      return false;
    }
  }

  if (root != get_root_ptr(cxt, coro_id)) {
    unlock_addr(lock_addr, tag, cas_buffer, cxt, coro_id, true);
      dsm->free(sibling_addr);
      return false;
  }

  write_page_and_unlock(page_buffer, page_addr, kLeafPageSize, cas_buffer,
                        lock_addr, tag, cxt, coro_id, need_split);

  auto up_level = path_stack[coro_id][level + 1];

  if (up_level != GlobalAddress::Null()) {
    internal_page_store(up_level, split_key, sibling_addr, root, level + 1, cxt,
                        coro_id);
  } else {
    assert(from_cache);
    insert_internal(split_key, sibling_addr, cxt, coro_id, level + 1);
  }

  return true;
}

bool Tree::leaf_page_del(GlobalAddress page_addr, const ShermanKey &k,
                         int level, CoroContext *cxt, int coro_id,
                         bool from_cache) {
  uint64_t lock_index =
      CityHash64((char *)&page_addr, sizeof(page_addr)) % define::kNumOfLock;

  GlobalAddress lock_addr;

#ifdef CONFIG_ENABLE_EMBEDDING_LOCK
  lock_addr = page_addr;
#else
  // lock_addr.nodeID = page_addr.nodeID;
  // lock_addr.offset = lock_index * sizeof(uint64_t);
  lock_addr.val = (uint64_t)&global_lock_table[lock_index];
#endif

  auto &rbuf = dsm->get_rbuf(coro_id);
  uint64_t *cas_buffer = rbuf.get_cas_buffer();
  auto page_buffer = rbuf.get_page_buffer();

  auto tag = dsm->getThreadTag();
  assert(tag != 0);

  lock_and_read_page(page_buffer, page_addr, kLeafPageSize, cas_buffer,
                     lock_addr, tag, cxt, coro_id);

  auto page = (LeafPage *)page_buffer;

  assert(page->hdr.level == level);
  assert(page->check_consistent());

  if (from_cache &&
      (k < page->hdr.lowest || k >= page->hdr.highest)) {  // cache is stale
    this->unlock_addr(lock_addr, tag, cas_buffer, cxt, coro_id, true);
    return false;
  }

  if (k >= page->hdr.highest) {
    this->unlock_addr(lock_addr, tag, cas_buffer, cxt, coro_id, true);
    assert(page->hdr.sibling_ptr != GlobalAddress::Null());
    this->leaf_page_del(page->hdr.sibling_ptr, k, level, cxt, coro_id);
    return true;
  }

  assert(k >= page->hdr.lowest);

  char *update_addr = nullptr;
  for (int i = 0; i < kLeafCardinality; ++i) {
    auto &r = page->records[i];
    if (r.key == k && r.value != kValueNull) {
      r.value = kValueNull;
      r.f_version++;
      r.r_version = r.f_version;
      update_addr = (char *)&r;
      break;
    }
  }

  if (update_addr) {
    write_page_and_unlock(
        update_addr, GADD(page_addr, (update_addr - (char *)page)),
        sizeof(LeafEntry), cas_buffer, lock_addr, tag, cxt, coro_id, false);
  } else {
    this->unlock_addr(lock_addr, tag, cas_buffer, cxt, coro_id, false);
  }
  return true;
}

void Tree::run_coroutine(CoroFunc func, int id, int coro_cnt) {
  using namespace std::placeholders;

  assert(coro_cnt <= define::kMaxCoro);
  for (int i = 0; i < coro_cnt; ++i) {
    auto gen = func(i, dsm, id);
    worker[i] = CoroCall(std::bind(&Tree::coro_worker, this, _1, gen, i));
  }

  master = CoroCall(std::bind(&Tree::coro_master, this, _1, coro_cnt));

  master();
}

void Tree::coro_worker(CoroYield &yield, RequstGen *gen, int coro_id) {
  CoroContext ctx;
  ctx.coro_id = coro_id;
  ctx.master = &master;
  ctx.yield = &yield;

  ShermanTimer coro_timer;
  auto thread_id = dsm->getMyThreadID();

  while (true) {
    auto r = gen->next();

    coro_timer.begin();
    if (r.is_search) {
      Value v;
      this->search(r.k, v, &ctx, coro_id);
    } else {
      this->insert(r.k, r.v, &ctx, coro_id);
    }
    auto us_10 = coro_timer.end() / 100;
    if (us_10 >= LATENCY_WINDOWS) {
      us_10 = LATENCY_WINDOWS - 1;
    }
    latency[thread_id][us_10]++;
  }
}

void Tree::coro_master(CoroYield &yield, int coro_cnt) {
  for (int i = 0; i < coro_cnt; ++i) {
    yield(worker[i]);
  }

  while (true) {
    uint64_t next_coro_id;

    if (!hot_wait_queue.empty()) {
      next_coro_id = hot_wait_queue.front();
      hot_wait_queue.pop();
      yield(worker[next_coro_id]);
    }
  }
}

// Local Locks
inline bool Tree::acquire_local_lock(GlobalAddress lock_addr, CoroContext *cxt,
                                     int coro_id) {
  auto &node = local_locks[reinterpret_cast<nt<uint64_t>*>(lock_addr.val) - global_lock_table];

  uint64_t lock_val = node.ticket_lock.fetch_add(1);

  uint32_t ticket = lock_val << 32 >> 32;
  uint32_t current = lock_val >> 32;

  while (ticket != current) {  // lock failed

    if (cxt != nullptr) {
      hot_wait_queue.push(coro_id);
      (*cxt->yield)(*cxt->master);
    }

    current = node.ticket_lock.load(std::memory_order_relaxed) >> 32;
  }

  node.hand_time++;

  return node.hand_over;
}

inline bool Tree::can_hand_over(GlobalAddress lock_addr) {
  auto &node = local_locks[reinterpret_cast<nt<uint64_t>*>(lock_addr.val) - global_lock_table];
  uint64_t lock_val = node.ticket_lock.load(std::memory_order_relaxed);

  uint32_t ticket = lock_val << 32 >> 32;
  uint32_t current = lock_val >> 32;

  if (ticket <= current + 1) {  // no pending locks
    node.hand_over = false;
  } else {
    node.hand_over = node.hand_time < define::kMaxHandOverTime;
  }
  if (!node.hand_over) {
    node.hand_time = 0;
  }

  return node.hand_over;
}

inline void Tree::release_local_lock(GlobalAddress lock_addr) {
  auto &node = local_locks[reinterpret_cast<nt<uint64_t>*>(lock_addr.val) - global_lock_table];

  node.ticket_lock.fetch_add((1ull << 32));
}

void Tree::register_thread(int cache_slot) {
  const int slot = cache_slot < 0 ? SimThreadInfo::worker_machine_id : cache_slot;
  index_cache = g_index_cache[slot];
  local_locks = g_local_locks[slot];
}

void Tree::index_cache_statistics() {
  index_cache->statistics();
  index_cache->bench();
}

void Tree::clear_statistics() {
  for (int i = 0; i < MAX_APP_THREAD; ++i) {
    cache_hit[i][0] = 0;
    cache_miss[i][0] = 0;
  }
}
