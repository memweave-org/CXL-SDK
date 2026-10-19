#if !defined(_INDEX_CACHE_H_)
#define _INDEX_CACHE_H_

#include "CacheEntry.h"
#include "Timer.h"
#include "WRLock.h"
#include "third_party/inlineskiplist.h"

#include <atomic>
#include <queue>
#include <vector>
#include <thread>
#include "../../cxl_transport/thread_epoch.h"

extern bool enter_debug;

using CacheSkipList = InlineSkipList<CacheEntryComparator>;

class IndexCache {

public:
  IndexCache(int cache_size);
  ~IndexCache() { stop_gc.store(true); if(gc_thread.joinable()) gc_thread.join(); }
  ThreadStatus thread_status{MAX_APP_THREAD};

  bool add_to_cache(InternalPage *page);
  const CacheEntry *search_from_cache(const ShermanKey &k, GlobalAddress *addr,
                                      bool is_leader = false);

  void search_range_from_cache(const ShermanKey &from, const ShermanKey &to,
                               std::vector<InternalPage *> &result);

  bool add_entry(const ShermanKey &from, const ShermanKey &to, InternalPage *ptr);
  const CacheEntry *find_entry(const ShermanKey &k);
  const CacheEntry *find_entry(const ShermanKey &from, const ShermanKey &to);

  bool invalidate(const CacheEntry *entry);

  const CacheEntry *get_a_random_entry(uint64_t &freq);

  void statistics();

  void bench();

private:
  std::atomic<bool> stop_gc{false};
  std::thread gc_thread;
  uint64_t cache_size; // MB;
  std::atomic<int64_t> free_page_cnt;
  std::atomic<int64_t> skiplist_node_cnt;
  int64_t all_page_cnt;

  std::queue<std::pair<void *, uint64_t>> delay_free_list;
  WRLock free_lock;

  // SkipList
  CacheSkipList *skiplist;
  CacheEntryComparator cmp;
  Allocator alloc;

  void evict_one();
};

inline IndexCache::IndexCache(int cache_size) : cache_size(cache_size) {
  skiplist = new CacheSkipList(cmp, &alloc, 21);
  uint64_t memory_size = define::MB * cache_size;

  all_page_cnt = memory_size / sizeof(InternalPage);
  free_page_cnt.store(all_page_cnt);
  skiplist_node_cnt.store(0);
  gc_thread=std::thread([this] {
    while(!stop_gc.load()) {
      std::queue<std::pair<void*,uint64_t>> retired;
      free_lock.wLock(); retired.swap(delay_free_list); free_lock.wUnlock();
      if(!retired.empty()) {
        thread_status.rcu_barrier();
        while(!retired.empty()) {
          free(retired.front().first); retired.pop(); free_page_cnt.fetch_add(1);
        }
      }
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
  });
}

// [from, to）
inline bool IndexCache::add_entry(const ShermanKey &from, const ShermanKey &to,
                                  InternalPage *ptr) {

  // TODO memory leak
  auto buf = skiplist->AllocateKey(sizeof(CacheEntry));
  auto &e = *(CacheEntry *)buf;
  e.from = from;
  e.to = to - 1; // !IMPORTANT;
  e.ptr = ptr;

  return skiplist->InsertConcurrently(buf);
}

inline const CacheEntry *IndexCache::find_entry(const ShermanKey &from,
                                                const ShermanKey &to) {
  CacheSkipList::Iterator iter(skiplist);

  CacheEntry e;
  e.from = from;
  e.to = to - 1;
  iter.Seek((char *)&e);
  if (iter.Valid()) {
    auto val = (const CacheEntry *)iter.key();
    return val;
  } else {
    return nullptr;
  }
}

inline const CacheEntry *IndexCache::find_entry(const ShermanKey &k) {
  return find_entry(k, k + 1);
}

inline bool IndexCache::add_to_cache(InternalPage *page) {
  auto new_page = (InternalPage *)malloc(kInternalPageSize);
  memcpy((void *)new_page, (void *)page, kInternalPageSize);
  new_page->index_cache_freq = 0;

  if (this->add_entry(page->hdr.lowest, page->hdr.highest, new_page)) {
    skiplist_node_cnt.fetch_add(1);
    auto v = free_page_cnt.fetch_add(-1);
    if (v <= 0) {
      evict_one();
    }

    return true;
  } else { // conflicted
    auto e = this->find_entry(page->hdr.lowest, page->hdr.highest);
    if (e && e->from == page->hdr.lowest && e->to == page->hdr.highest - 1) {
      auto ptr = e->ptr;
      if (ptr == nullptr &&
          __sync_bool_compare_and_swap(&(e->ptr), 0ull, new_page)) {
        auto v = free_page_cnt.fetch_add(-1);
        if (v <= 0) {
          evict_one();
        }
        return true;
      }
    }

    free(new_page);
    return false;
  }
}

inline const CacheEntry *IndexCache::search_from_cache(const ShermanKey &k,
                                                       GlobalAddress *addr,
                                                       bool is_leader) {
  // Retired snapshots are reclaimed by the epoch-protected collector.
  auto entry = find_entry(k);

  InternalPage *page = entry ? entry->ptr : nullptr;

  if (page && entry->from <= k && entry->to >= k) {

    page->index_cache_freq++;

    auto cnt = page->hdr.last_index + 1;
    if (k < page->records[0].key) {
      *addr = page->hdr.leftmost_ptr;
    } else {

      bool find = false;
      for (int i = 1; i < cnt; ++i) {
        if (k < page->records[i].key) {
          find = true;
          *addr = page->records[i - 1].ptr;
          break;
        }
      }
      if (!find) {
        *addr = page->records[cnt - 1].ptr;
      }
    }

    compiler_barrier();
    if (entry->ptr) { // check if it is freed.
      return entry;
    }
  }

  return nullptr;
}

inline void
IndexCache::search_range_from_cache(const ShermanKey &from, const ShermanKey &to,
                                    std::vector<InternalPage *> &result) {
  CacheSkipList::Iterator iter(skiplist);

  result.clear();
  CacheEntry e;
  e.from = from;
  e.to = from;
  iter.Seek((char *)&e);

  while (iter.Valid()) {
    auto val = (const CacheEntry *)iter.key();
    if (val->ptr) {
      if (val->from > to) {
        return;
      }
      result.push_back(val->ptr);
    }
    iter.Next();
  }
}

inline bool IndexCache::invalidate(const CacheEntry *entry) {
  auto ptr = entry->ptr;

  if (ptr == nullptr) {
    return false;
  }

  if (__sync_bool_compare_and_swap(&(entry->ptr), ptr, 0)) {

    free_lock.wLock();
    delay_free_list.push(std::make_pair(ptr, asm_rdtsc()));
    free_lock.wUnlock();
    return true;
  }

  return false;
}

inline const CacheEntry *IndexCache::get_a_random_entry(uint64_t &freq) {
  thread_local uint32_t seed = asm_rdtsc();
  GlobalAddress tmp_addr;
  for(int attempt=0;attempt<64;++attempt) {
    // Cover the actual 64-bit key space; the original 1-billion-key sampling
    // range can contain no live entry in a sparse random-key benchmark.
    uint64_t k=(uint64_t(rand_r(&seed))<<33) ^
               (uint64_t(rand_r(&seed))<<2) ^ (rand_r(&seed)&3);
    auto e=search_from_cache(k,&tmp_addr);
    if(!e) continue;
    auto ptr=e->ptr;
    if(!ptr) continue;
    freq=ptr->index_cache_freq;
    if(e->ptr==ptr) return e;
  }
  return nullptr;
}

inline void IndexCache::evict_one() {
  uint64_t freq1=0, freq2=0;
  auto e1=get_a_random_entry(freq1), e2=get_a_random_entry(freq2);
  if(e1 && (!e2 || freq1<freq2)) invalidate(e1);
  else if(e2) invalidate(e2);
}

inline void IndexCache::statistics() {
  printf("[skiplist node: %ld]  [page cache: %ld]\n", skiplist_node_cnt.load(),
         all_page_cnt - free_page_cnt.load());
}

inline void IndexCache::bench() {

  ShermanTimer t;
  t.begin();
  const int loop = 100000;

  for (int i = 0; i < loop; ++i) {
    uint64_t r = rand() % (5 * define::MB);
    this->find_entry(r);
  }

  t.end_print(loop);
}

#endif // _INDEX_CACHE_H_
