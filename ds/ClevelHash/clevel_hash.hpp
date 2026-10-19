#pragma once

#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <new>
#include <thread>

#include "shm/cxl_type.h"
#include "utils/atomic_pointer.h"
#include "utils/atomic_variable.h"
#include "utils/bypass_cache.h"
#include "utils/sim_id.h"
#include "utils/timing.h"
#include "replica_help_update/help_update.h"

// #define OPT_NO_META
// #define OPT_BATCH_FLUSH

// #define OPT_CLEVEL_ROOT_READ

#define MAX_LEVEL 16

// #define CLEVEL_DEBUG 1

/**
 * The builtin performs an atomic compare and swap. That is, if the
 * current value of *ptr is oldval, then write newval into *ptr.
 * Return true if the comparison is successful and newval was written.
 */
#define CAS(ptr, oldval, newval)                                               \
  (__sync_bool_compare_and_swap(ptr, oldval, newval))

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

// #if !LIBPMEMOBJ_CPP_USE_TBB_RW_MUTEX
// using internal::shared_mutex_scoped_lock;
// #endif

#ifdef NO_CC
template <typename T> using atomic_type = nt<T>;
#else
template <typename T> using atomic_type = std::atomic<T>;
#endif
struct hash64shift {
  size_t operator()(uint64_t key) const {
    key = (~key) + (key << 21); // key = (key << 21) - key - 1;
    key = key ^ (key >> 24);
    key = (key + (key << 3)) + (key << 8); // key * 265
    key = key ^ (key >> 14);
    key = (key + (key << 2)) + (key << 4); // key * 21
    key = key ^ (key >> 28);
    key = key + (key << 31);
    return key;
  }
};

template <typename Key, typename T, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>, size_t HashPower = 20>
class clevel_hash {
public:
  using key_type = Key;
  using mapped_type = T;
  using value_type = std::pair<const Key, T>;
  using size_type = size_t;
  using difference_type = ptrdiff_t;
  using pointer = value_type *;
  using const_pointer = const value_type *;
  using reference = value_type &;
  using const_reference = const value_type &;

  // using hasher = Hash;
  using hasher =
      hash64shift; // Use a different hash function can be much faster for
                   // consecutive keys, because it prevents a lot of finds!
  using key_equal = KeyEqual;

  using hv_type = size_t;
  using partial_t = uint16_t;

  typedef enum FindCode {
    ABSENT_AND_NO_VACANCY = 0,
    FOUND_IN_LEFT = 1,
    FOUND_IN_RIGHT = 2,
    VACANCY_IN_LEFT = 3,
    VACANCY_IN_RIGHT = 4,
  } f_code_t;

  struct level_bucket;
  struct level_meta;

  // using KV_entry_ptr_t = nt_pointer<value_type>;

  using level_ptr_t = level_bucket *;

  #ifdef NO_CC
  using level_meta_ptr_t = nt_pointer<level_meta>;
  #else
  using level_meta_ptr_t = std::atomic<level_meta *>;
  #endif

  constexpr static size_type assoc_num = 4;
  constexpr static size_type resize_bulk = 1;

  constexpr static size_type partial_ext_bits =
      (sizeof(uint64_t) - sizeof(partial_t)) * 8;

  difference_type first_index(hv_type hv, size_type capacity) const {
    // Since the "bucket_idx" needs to be "std::ptrdiff_t" due to
    // the requirement in persistent_ptr<bucket[]>, so we adopt
    // "difference_type" (i.e., "std::ptrdiff_t") as the data type
    // of bucket index.
    return static_cast<difference_type>(hv % (capacity / 2));
  }

  difference_type second_index(partial_t partial, difference_type idx,
                               size_type capacity) const {
    partial_t nonzero_tag = (partial >> 1 << 1) + 1;
    // 0xc6a4a7935bd1e995 is the hash constant from 64-bit MurmurHash2
    uint64_t hash_of_tag = (uint64_t)(nonzero_tag * 0xc6a4a7935bd1e995);
    return static_cast<difference_type>(
        (static_cast<uint64_t>(idx) ^ hash_of_tag) % (capacity / 2) +
        capacity / 2);
  }

  difference_type alt_index(partial_t partial, difference_type idx,
                            size_type capacity) const {
    partial_t nonzero_tag = (partial >> 1 << 1) + 1;
    // 0xc6a4a7935bd1e995 is the hash constant from 64-bit MurmurHash2
    uint64_t hash_of_tag = (uint64_t)(nonzero_tag * 0xc6a4a7935bd1e995);
    if (static_cast<size_type>(idx) < (capacity / 2)) {
      return static_cast<difference_type>(
          (static_cast<uint64_t>(idx) ^ hash_of_tag) % (capacity / 2) +
          capacity / 2);
    } else {
      return static_cast<difference_type>(
          (static_cast<uint64_t>(idx) ^ hash_of_tag) % (capacity / 2));
    }
  }

  struct ret {
    bool found;
    uint8_t level_idx;
    difference_type bucket_idx;
    int8_t slot_idx;
    bool expanded;
    uint64_t capacity;
    value_type *value;

    ret(size_type _level_idx, difference_type _bucket_idx, size_type _slot_idx,
        bool _expanded = false, uint64_t _cap = 0)
        : found(true), level_idx(_level_idx), bucket_idx(_bucket_idx),
          slot_idx(_slot_idx), expanded(_expanded), capacity(_cap) {}

    ret(size_type _level_idx, difference_type _bucket_idx, size_type _slot_idx,
        value_type *_value, bool _expanded = false, uint64_t _cap = 0)
        : found(true), level_idx(_level_idx), bucket_idx(_bucket_idx),
          slot_idx(_slot_idx), expanded(_expanded), capacity(_cap),
          value(_value) {}

    ret(bool _expanded, uint64_t _cap)
        : found(false), level_idx(0), bucket_idx(0), slot_idx(0),
          expanded(_expanded), capacity(_cap) {}

    ret(bool _found)
        : found(_found), level_idx(0), bucket_idx(0), slot_idx(0),
          expanded(false), capacity(0) {}

    ret()
        : found(false), level_idx(0), bucket_idx(0), slot_idx(0),
          expanded(false), capacity(0) {}
  };

  struct KV_entry_ptr_s;

  struct KV_entry_ptr_u {
    atomic_type<uint64_t> p;
    KV_entry_ptr_u() : p(0) {}
    KV_entry_ptr_u(atomic_type<uint64_t> &ptr) : p(ptr.load()) {}
    KV_entry_ptr_u(value_type *ptr) : p((uint64_t)ptr) {}

#ifndef NO_CC
    KV_entry_ptr_u(const KV_entry_ptr_u &ptr) : p(ptr.p.load()) {}
#endif

    ~KV_entry_ptr_u() {}

    inline KV_entry_ptr_u &operator=(const KV_entry_ptr_s &ptr) {
      p.store((uint64_t)ptr.p, std::memory_order_relaxed);
      return *this;
    }

#ifdef NO_CC
    inline value_type *addr(bool nt = true) {
      return (value_type *)(p.load(std::memory_order_seq_cst, nt) &
                            0xFFFFFFFFFFFF);
    }

    inline partial_t partial(bool nt = true) {
      return p.load(std::memory_order_seq_cst, nt) >> 48;
    }

    inline void set_partial(partial_t par) {
      p.store((p & 0xFFFFFFFFFFFF) | ((uint64_t)par << 48),
              std::memory_order_seq_cst, false);
    }
#else
    inline value_type *addr(bool nt = true) {
      return (value_type *)(p.load(std::memory_order_relaxed) & 0xFFFFFFFFFFFF);
    }

    inline partial_t partial(bool nt = true) {
      return p.load(std::memory_order_relaxed) >> 48;
    }

    inline void set_partial(partial_t par) {
      p.store((p & 0xFFFFFFFFFFFF) | ((uint64_t)par << 48),
              std::memory_order_relaxed);
    }
    inline KV_entry_ptr_u &operator=(const KV_entry_ptr_u &ptr) {
      p.store((uint64_t)ptr.p.load(std::memory_order_relaxed),
              std::memory_order_relaxed);
      return *this;
    }
#endif

    inline bool operator==(const KV_entry_ptr_u &other) const {
      return p == other.p;
    }

    inline bool operator!=(const KV_entry_ptr_u &other) const {
      return p != other.p;
    }
  };

  struct KV_entry_ptr_s {
    uint64_t p;
    KV_entry_ptr_s() : p(0) {}
    KV_entry_ptr_s(const KV_entry_ptr_u &ptr) : p(ptr.p.load()) {}

    KV_entry_ptr_s(atomic_type<uint64_t> &ptr) : p(ptr.load()) {}

    inline KV_entry_ptr_s &operator=(const KV_entry_ptr_s &ptr) {
      p = (uint64_t)ptr.p;
      return *this;
    }

    KV_entry_ptr_s(value_type *ptr) : p((uint64_t)ptr) {}

    ~KV_entry_ptr_s() {}

    inline value_type *addr(bool nt = true) {
      return (value_type *)(p & 0xFFFFFFFFFFFF);
    }

    inline partial_t partial(bool nt = true) { return p >> 48; }

    inline void set_partial(partial_t par) {
      p = (p & 0xFFFFFFFFFFFF) | ((uint64_t)par << 48);
    }

    inline bool operator==(const KV_entry_ptr_s &other) const {
      return p == other.p;
    }

    inline bool operator!=(const KV_entry_ptr_s &other) const {
      return p != other.p;
    }
  };

  struct bucket {
    KV_entry_ptr_u slots[assoc_num];
  };

  struct level_bucket {
    struct bucket *buckets;
    uint64_t capacity;
    atomic_type<level_ptr_t> up;
    // std::atomic<level_ptr_t> up;

    level_bucket() : buckets(nullptr), capacity(0), up(nullptr) {}

    void allocate(uint64_t capacity) {
#ifdef USE_CXL
      this->buckets = new (cacheable.malloc(capacity * sizeof(bucket))) bucket[capacity];
      if (this->buckets == nullptr) {
        printf("Failed to allocate memory for level_bucket, size=%lu\n", capacity);
        exit(1);
      }
#else
      this->buckets = new bucket[capacity];
#endif
    }

    void clear() {
#ifdef USE_CXL
      cacheable.free(buckets);
#else
      delete[] buckets;
#endif
    }
  };

  struct level_meta {
    level_ptr_t first_level;
    level_ptr_t last_level;
    char is_resizing;
#ifdef OPT_CLEVEL_ROOT_READ
    int level_count;
    level_ptr_t levels[MAX_LEVEL];
#endif

    level_meta() {
      first_level = nullptr;
      last_level = nullptr;
      is_resizing = false;
#ifdef OPT_CLEVEL_ROOT_READ
      level_count = 0;
      for (int i = 0; i < MAX_LEVEL; i++) {
        levels[i] = nullptr;
      }
#endif
    }

    level_meta(const level_ptr_t &fl, const level_ptr_t &ll, bool flag) {
      first_level = fl;
      last_level = ll;
      is_resizing = flag;
#ifdef OPT_CLEVEL_ROOT_READ
      set_levels();
#endif
    }

#ifdef OPT_CLEVEL_ROOT_READ
    void set_levels() {
      level_ptr_t l = last_level;
      int i = 0;
      // printf("set_levels: first_level = %p, last_level = %p\n", first_level, last_level);
      while (l != first_level && i < MAX_LEVEL - 1) {
        // printf("set_levels: i = %d, l = %p, l->capacity = %lu\n", i, l, l->capacity);
        levels[i] = l;
        level_ptr_t next = l->up.load();
        if (next == nullptr) {
          break;
        }
        l = next;
        i++;
      }
      
      // Ensure first_level is also placed in the array (if not already)
      if (l == first_level && i < MAX_LEVEL) {
        // If loop ends normally (l == first_level), place first_level in array
        levels[i] = first_level;
        // printf("set_levels: i = %d, l = %p, l->capacity = %lu\n", i, l, l->capacity);
        level_count = i + 1;
      } else if (i < MAX_LEVEL) {
        // If loop exits for other reasons (e.g., reached MAX_LEVEL), ensure first_level is in array
        levels[i] = first_level;
        // printf("set_levels: i = %d, l = %p, l->capacity = %lu\n", i, l, l->capacity);
        level_count = i + 1;
      } else {
        // If exceeds MAX_LEVEL, at least ensure first_level is at the end of array
        levels[MAX_LEVEL - 1] = first_level;
        level_count = MAX_LEVEL;
      }
    }
#endif
  };

  static partial_t get_partial(hv_type hv) {
    constexpr static size_type shift_bits =
        (sizeof(hv_type) - sizeof(partial_t)) * 8;
    return (partial_t)((uint64_t)hv >> shift_bits);
  }

#ifdef USE_CXL
  clevel_hash() : meta(new (cacheable.malloc(sizeof(level_meta))) level_meta()), thread_num(0) {
#else
  clevel_hash() : meta(new level_meta()), thread_num(0) {
#endif
    std::cout << "clevel_hash constructor: HashPower = " << HashPower
              << std::endl;

    assert(HashPower > 0);
    hashpower = HashPower;

    std::cout << "hashpower : " << hashpower << std::endl;

#ifdef CLEVEL_DOUBLE_READ_COUNT
    double_read_count = new CallCounter("clevel_double_read_count");
    next_level_count = new CallCounter("clevel_next_level_count");
#endif

    level_meta *m = meta.load();

    level_bucket *tmp;
    size_t capacity;

#ifdef USE_CXL
    tmp = new (cacheable.malloc(sizeof(level_bucket))) level_bucket();
#else
    tmp = new level_bucket();
#endif
    capacity = 1 << hashpower;
    tmp->allocate(capacity);
    tmp->capacity = capacity;
    tmp->up = nullptr;
    m->first_level = tmp;

#ifdef USE_CXL
    tmp = new (cacheable.malloc(sizeof(level_bucket))) level_bucket();
#else
    tmp = new level_bucket();
#endif
    capacity = 1 << (hashpower - 1);
    tmp->allocate(capacity);
    tmp->capacity = capacity;
    tmp->up = m->first_level;
    m->last_level = tmp;

    m->is_resizing = false;

    run_expand_thread.store(true);
    expand_thread = std::thread(&clevel_hash::resize, this);

    KV_entry_ptr_s e = get_entry(meta.load()->first_level, 0, 0);
    if (e.addr() != nullptr) {
      // never fires.
      get_key(e);
    }
  }

  static void destroy_shared_value(value_type *value) {
    if (value) {
      value->~value_type();
      cacheable.free(value);
    }
  }

  static void allocate_KV_copy_construct(value_type *&KV_ptr,
                                         const void *param) {
    const value_type *tmp_value = static_cast<const value_type *>(param);
    value_type *real_value = new (cacheable.malloc(sizeof(value_type))) value_type(*tmp_value);
    KV_ptr = real_value;
  }

  // static void allocate_KV_move_construct(nt_pointer<value_type> &KV_ptr,
  //                                        const void *param) {
  //   const value_type *const_param = static_cast<const value_type *>(param);
  //   value_type *real_value = const_cast<value_type *>(const_param);
  //   KV_ptr.store(real_value);
  // }

  ret insert(const value_type &value, size_type thread_id, size_type id) {
    return generic_insert(value.first, &value, allocate_KV_copy_construct,
                          thread_id, id);
  }

  // ret insert(value_type &&value, size_type thread_id, size_type id) {
  //   return generic_insert(value.first, &value, allocate_KV_move_construct,
  //                         thread_id, id);
  // }

  ret generic_insert(const key_type &key, const void *param,
                     void (*allocate_KV)(value_type *&, const void *),
                     size_type thread_id, size_type id);

  // mapped_type
  ret search(const key_type &key) const;

  ret erase(const key_type &key, size_type thread_id);

  ret update(const value_type &value, size_type thread_id) {
    return generic_update(value.first, &value, allocate_KV_copy_construct,
                          thread_id);
  }

  // value is a universal reference
  // ret update(value_type &&value, size_type thread_id) {
  //   return generic_update(value.first, &value, allocate_KV_move_construct,
  //                         thread_id);
  // }

  ret generic_update(const key_type &key, const void *param,
                     void (*allocate_KV)(value_type *&, const void *),
                     size_type thread_id);

  void clear();

  ~clevel_hash() {
    run_expand_thread.store(false);
    expand_thread.join();
    clear();
  }

  // for debug
  void foo() {
    std::cout << "clevel_hash::foo()" << std::endl;
    std::cout << "sizeof(KV_entry_ptr_u) = " << sizeof(KV_entry_ptr_u)
              << std::endl;
    std::cout << "sizeof(meta) = " << sizeof(meta) << std::endl;
  }

  uint64_t capacity() const { return capacity(meta.load()); }

  /**
   * Get the total capacity (#buckets * assoc_num) of given context.
   */
  uint64_t capacity(level_meta_ptr_t m_copy) const {
    level_meta *m = m_copy;

    uint64_t total_slots = 0;
    level_ptr_t li;
#ifdef OPT_CLEVEL_ROOT_READ
    for (int i = 0; i < m->level_count; i++) {
      li = m->levels[i];
      total_slots += li->capacity * assoc_num;
    }
#else
    for (li = m->last_level; li != m->first_level;) {
      total_slots += li->capacity * assoc_num;
      li = li->up;
    }
    total_slots += li->capacity * assoc_num;
#endif
    return total_slots;
  }

  void set_thread_num(size_type num) {
    if (thread_num > 0) {
      // Reclaim the memory in persistent buffers allocated in previous
      // round of set_thread_num.
      tmp_meta.clear();
      tmp_level.clear();
      tmp_entry.clear();

#ifdef OPT_CLEVEL_ROOT_READ
      meta_replicas.clear();
#endif
    }

    thread_num = num + 1;

#ifdef CLEVEL_DEBUG
    thread_logs.resize(thread_num);
    for (uint64_t i = 0; i < thread_num; i++) {
      if (!thread_logs[i].is_open()) {
        std::stringstream ss;
        ss << "thread-" << i << ".log";
        thread_logs[i].open(ss.str(), std::fstream::out);
      }
    }
#endif

    // Setup persistent buffers according to the thread_num.
    tmp_meta.resize(thread_num);
    tmp_level.resize(thread_num);
    tmp_entry.resize(thread_num);
#ifdef OPT_CLEVEL_ROOT_READ
    meta_replicas.resize(thread_num);
    // replica_ptrs has fixed size, no need to resize
    for (size_type i = 0; i < thread_num; i++) {
      meta_replicas[i].store(meta.load());
    }
    help_update = new (cacheable.malloc(sizeof(HelpUpdate<level_meta>))) HelpUpdate<level_meta>(meta, meta_replicas, thread_num);
#endif
  }

  // Only for debug use!
  KV_entry_ptr_u &get_entry(level_ptr_t level, difference_type idx,
                            uint64_t slot_idx);

  // Only for debug use!
  key_type get_key(KV_entry_ptr_u &e);
  key_type get_key(KV_entry_ptr_s &e);

  void del_dup(KV_entry_ptr_u *p1, KV_entry_ptr_u *p2, KV_entry_ptr_s e1,
               KV_entry_ptr_s e2);

  f_code_t find(const key_type &key, partial_t partial, size_type &n_levels,
                KV_entry_ptr_s &old_e, KV_entry_ptr_u **e, uint64_t &level_num,
                difference_type &idx, bool fix_dup, size_type thread_id,
                level_meta *m_copy,
                KV_entry_ptr_u **last_bucket_entry = nullptr);

  f_code_t find_empty_slot(const key_type &key, partial_t partial,
                           size_type &n_levels, KV_entry_ptr_u **e,
                           uint64_t &level_num, level_meta *&m_copy);

  void expand(size_type thread_id, level_meta *m_copy);

  void resize();

  level_meta_ptr_t meta;
#ifdef OPT_CLEVEL_ROOT_READ
  cxl_vector<level_meta_ptr_t> meta_replicas;
  HelpUpdate<level_meta> *help_update;
#endif

  struct alignas(CACHE_LINE_SIZE) aligned_level_meta_ptr_t {
    level_meta_ptr_t meta;
    aligned_level_meta_ptr_t() : meta(nullptr) {}
    aligned_level_meta_ptr_t(level_meta_ptr_t m) : meta(m) {}
    aligned_level_meta_ptr_t(const aligned_level_meta_ptr_t &m)
        : meta(m.meta) {}
    aligned_level_meta_ptr_t &operator=(const aligned_level_meta_ptr_t &m) {
      meta = m.meta;
      return *this;
    }
    aligned_level_meta_ptr_t &operator=(level_meta_ptr_t m) {
      meta = m;
      return *this;
    }
    operator level_meta_ptr_t() { return meta; }
  };

  size_type hashpower;
  atomic_type<size_type> thread_num;
  atomic_type<char> run_expand_thread;
  // Array for each thread
  cxl_vector<level_meta *> tmp_meta;
  cxl_vector<level_bucket *> tmp_level;
  cxl_vector<value_type *> tmp_entry;

  std::thread expand_thread;

#ifdef CLEVEL_DOUBLE_READ_COUNT
  CallCounter *double_read_count;
  CallCounter *next_level_count;
#endif

#ifdef CLEVEL_DEBUG
  std::vector<std::fstream> thread_logs;
#endif
};

template <typename Key, typename T, typename Hash, typename KeyEqual,
          size_t HashPower>
typename clevel_hash<Key, T, Hash, KeyEqual, HashPower>::ret
clevel_hash<Key, T, Hash, KeyEqual, HashPower>::search(
    const key_type &key) const {
  hv_type hv = hasher{}(key);
  partial_t partial = get_partial(hv);


#ifdef OPT_CLEVEL_ROOT_READ
  // printf("worker_thread_id = %zu\n", SimThreadInfo::worker_thread_id);
  level_meta *m = help_update->load_ptr(SimThreadInfo::worker_thread_id);
#else
  level_meta *m = meta.load();
#endif

  while (true) {
  RETRY_READ:
    // Bottom-to-top search.
#ifdef OPT_CLEVEL_ROOT_READ
    int level_idx = 0;
    level_bucket *li = nullptr, *next_li = m->levels[level_idx];
#else
    level_bucket *li = nullptr, *next_li = m->last_level;
#endif

    difference_type f_idx, s_idx;
    size_type i = 0;
    do {
      li = next_li;
      level_bucket *cl = li;

      f_idx = first_index(hv, cl->capacity);
      s_idx = second_index(partial, f_idx, cl->capacity);

      bucket &f_b = cl->buckets[f_idx];
      bucket &s_b = cl->buckets[s_idx];

      // Flush two buckets simultaneously to reduce the overhead.
      for (size_type j = 0; j < assoc_num; j++) {
        KV_entry_ptr_s slot = f_b.slots[j];
        if (slot.partial() == partial && slot.addr() != nullptr) {
          if (key_equal{}(slot.addr()->first, key)) {
            return ret(i, f_idx, j, slot.addr());
          }
        }
      }

#ifdef CLEVEL_DOUBLE_READ_COUNT
      double_read_count->Increment();
#endif
      for (size_type j = 0; j < assoc_num; j++) {
        KV_entry_ptr_s slot = s_b.slots[j];
        if (slot.partial() == partial && slot.addr() != nullptr) {
          if (key_equal{}(slot.addr()->first, key)) {
            return ret(i, s_idx, j, slot.addr());
          }
        }
      }

#ifdef OPT_CLEVEL_ROOT_READ
      level_idx++;
      if (level_idx >= m->level_count) {
        break;
      }
      next_li = m->levels[level_idx];
      assert(next_li != nullptr);
#else
      next_li = cl->up;
#endif

#ifdef CLEVEL_DOUBLE_READ_COUNT
      next_level_count->Increment();
#endif
      i++;
    } while (li != m->first_level);

    // Context checking.
    level_meta *tmp_meta;
#ifdef OPT_CLEVEL_ROOT_READ
    tmp_meta = help_update->load_ptr(SimThreadInfo::worker_thread_id);
#else
    tmp_meta = meta.load();
#endif
    if (tmp_meta == m) {
      return ret();
    } else {
      m = tmp_meta;
    }
  } // end while(true)
}

template <typename Key, typename T, typename Hash, typename KeyEqual,
          size_t HashPower>
void clevel_hash<Key, T, Hash, KeyEqual, HashPower>::del_dup(
    KV_entry_ptr_u *p1, KV_entry_ptr_u *p2, KV_entry_ptr_s e1,
    KV_entry_ptr_s e2) {
  KV_entry_ptr_s tmp1_u{e1}, tmp2_u{e2};

  if (e1 != *p1 || e2 != *p2)
    return;

  if (tmp1_u.partial(true) == tmp2_u.partial(true)) {
    // 1. Refer to the same location
    if (e1 == e2) {
      uint64_t expected = e2.p;
      p2->p.compare_exchange_strong(expected, 0);
    }

    // 2. Refer to different locations with the same contents
    else if (key_equal{}(e1.addr()->first, e2.addr(true)->first)) {
      uint64_t expected = e2.p;
      bool ret = p2->p.compare_exchange_strong(expected, 0);
      if (ret) {
        destroy_shared_value(e2.addr());
      }
    }
  }
}

template <typename Key, typename T, typename Hash, typename KeyEqual,
          size_t HashPower>
typename clevel_hash<Key, T, Hash, KeyEqual, HashPower>::f_code_t
clevel_hash<Key, T, Hash, KeyEqual, HashPower>::find_empty_slot(
    const key_type &key, partial_t partial, size_type &n_levels,
    KV_entry_ptr_u **e, uint64_t &level_num, level_meta *&m_copy) {
  hv_type hv = hasher{}(key);
  while (true) {
    level_meta *m = m_copy;
    *e = nullptr;

    level_bucket *levels[MAX_LEVEL];
    difference_type f_idx, s_idx;
    uint64_t slot_idx;

    f_code_t result;

#ifdef OPT_CLEVEL_ROOT_READ
    n_levels = m->level_count;
    for (size_type i = 0; i < n_levels && i < MAX_LEVEL; i++) {
      levels[i] = m->levels[i];
    }
#else
    n_levels = 0;
    level_bucket *li = nullptr, *next_li = m->last_level;
    do {
      li = next_li;
      levels[n_levels] = li;
      n_levels++;
      next_li = li->up;
    } while (li != m->first_level);
#endif

    level_bucket *cl;
    result = ABSENT_AND_NO_VACANCY;

    for (size_type i = n_levels - 1; i < n_levels; i--) {
      cl = levels[i];
      f_idx = first_index(hv, cl->capacity);
      s_idx = second_index(partial, f_idx, cl->capacity);

      // flag used to skip vacant slots after finding an empty
      // slot in a bucket.
      bool found_empty_in_b = false;
      bucket &f_b = cl->buckets[f_idx];
      bucket &s_b = cl->buckets[s_idx];
      for (size_type j = 0; j < assoc_num; j++) {
        if (!found_empty_in_b && f_b.slots[j].addr() == nullptr) {
          found_empty_in_b = true;

          result = VACANCY_IN_LEFT;
          *e = &(f_b.slots[j]);
          level_num = i;
          slot_idx = j;
        }
      }

      // flag used to skip vacant slots after finding an empty
      // slot in a bucket.
      found_empty_in_b = false;
      for (size_type j = 0; j < assoc_num; j++) {
        if (!found_empty_in_b && s_b.slots[j].addr() == nullptr) {
          found_empty_in_b = true;

          // We prefer the less loaded bucket
          if (result == VACANCY_IN_LEFT && level_num == i && slot_idx <= j)
            continue;

          result = VACANCY_IN_RIGHT;
          *e = &(s_b.slots[j]);
          level_num = i;
          slot_idx = j;
        }
      }

      if (result != ABSENT_AND_NO_VACANCY)
        break;
    }

    // Context checking.
    level_meta *tmp_meta;
#ifdef OPT_CLEVEL_ROOT_READ
    tmp_meta = help_update->load_ptr(SimThreadInfo::worker_thread_id);
#else
    tmp_meta = meta.load();
#endif
    if (m_copy == tmp_meta) {
      return result;
    } else {
      m_copy = tmp_meta;
    }
  }
}

template <typename Key, typename T, typename Hash, typename KeyEqual,
          size_t HashPower>
typename clevel_hash<Key, T, Hash, KeyEqual, HashPower>::f_code_t
clevel_hash<Key, T, Hash, KeyEqual, HashPower>::find(
    const key_type &key, partial_t partial, size_type &n_levels,
    KV_entry_ptr_s &old_e, KV_entry_ptr_u **e, uint64_t &level_num,
    difference_type &idx, bool fix_dup, size_type thread_id, level_meta *m_copy,
    KV_entry_ptr_u **last_bucket_entry) {
  hv_type hv = hasher{}(key);

  while (true) {
  RETRY_FIND:
    level_meta *m = m_copy;
    *e = nullptr;

    level_ptr_t levels[MAX_LEVEL];
    difference_type f_idx, s_idx;
    KV_entry_ptr_s f_e, s_e;
    uint64_t slot_idx;

    f_code_t result;
    KV_entry_ptr_s prev_e;
    size_type prev_i;

#ifdef OPT_CLEVEL_ROOT_READ
    n_levels = m->level_count;
    for (size_type i = 0; i < n_levels && i < MAX_LEVEL; i++) {
      levels[i] = m->levels[i];
    }
#else
    n_levels = 0;
    level_ptr_t next_li = m->last_level, li = nullptr;
    do {
      li = next_li;
      levels[n_levels] = li;
      n_levels++;
      next_li = li->up;
    } while (li != m->first_level);
#endif
    level_bucket *cl;
    result = ABSENT_AND_NO_VACANCY;

    // Bottom-to-top search.
    for (size_type i = 0; i < n_levels; i++) {
      cl = levels[i];
      uint64_t capacity = cl->capacity;
      f_idx = first_index(hv, capacity);
      s_idx = second_index(partial, f_idx, capacity);

      // flag used to skip vacant slots after finding an empty slot
      // in a bucket.
      bool found_empty_in_b = false;
      bucket &f_b = cl->buckets[f_idx];
      bucket &s_b = cl->buckets[s_idx];
      for (size_type j = 0; j < assoc_num; j++) {
        f_e = f_b.slots[j];
        if (f_e.addr() == nullptr) {
          // Since empty slots in top levels are preferred, update
          // vacancy info as long as identical keys are not found.
          if (result != FOUND_IN_LEFT && result != FOUND_IN_RIGHT &&
              !found_empty_in_b) {
            found_empty_in_b = true;

            result = VACANCY_IN_LEFT;
            old_e = f_e;
            *e = &(f_b.slots[j]);
            if (last_bucket_entry != nullptr && f_idx != 0) {
              *last_bucket_entry = &(cl->buckets[f_idx - 1].slots[0]);
            }
            level_num = i;
            idx = f_idx;
            slot_idx = j;
          }
          continue;
        }

        if (f_e.partial() != partial || !key_equal{}(f_e.addr()->first, key))
          continue;

        if (!fix_dup) {
          result = FOUND_IN_LEFT;
          level_num = i;
          idx = f_idx;
          slot_idx = j;

          return result;
        }

        if (result == FOUND_IN_LEFT || result == FOUND_IN_RIGHT) {
          // Refer to the same location
          if (f_e == prev_e) {
            // Duplication due to the re-insertion in normal
            // executions or os scheduling during a rehashing
            // operation. In this case, delete the pointer in
            // bottom level.
            if (prev_i < i) {
              del_dup(&f_b.slots[j],
                      &(levels[level_num]->buckets[idx].slots[slot_idx]), f_e,
                      prev_e);
            } else {
              // Never fires!
              assert(false);
            }
          }
          // Refer to different locations
          else {
            // Duplication due to the re-insertion after a crash
            // or concurrent insertions of same key. To fix the
            // duplication, simply delete the previous item.
            del_dup(&f_b.slots[j],
                    &(levels[level_num]->buckets[idx].slots[slot_idx]), f_e,
                    prev_e);
          }
          goto RETRY_FIND;
        } else {
          result = FOUND_IN_LEFT;
          old_e = f_e;
          *e = &(f_b.slots[j]);
          level_num = i;
          idx = f_idx;
          slot_idx = j;
          if (last_bucket_entry != nullptr && f_idx != 0) {
            *last_bucket_entry = &(cl->buckets[f_idx - 1].slots[0]);
          }

          prev_e = f_e;
          prev_i = i;
        } // end if result in FOUND_IN_LEFT or FOUND_IN_RIGHT
      } // end for j, f_idx, f_b

      found_empty_in_b = false;
      for (size_type j = 0; j < assoc_num; j++) {
        s_e = s_b.slots[j];
        if (s_e.addr() == nullptr) {
          // Since empty slots in top levels are preferred, update
          // vacancy info as long as identical keys are not found.
          if (result != FOUND_IN_LEFT && result != FOUND_IN_RIGHT &&
              !found_empty_in_b) {
            found_empty_in_b = true;

            // We prefer the less loaded bucket
            if (result == VACANCY_IN_LEFT && level_num == i && slot_idx <= j)
              continue;

            result = VACANCY_IN_RIGHT;
            old_e = s_e;
            *e = &(s_b.slots[j]);
            if (last_bucket_entry != nullptr && s_idx != 0) {
              *last_bucket_entry = &(cl->buckets[s_idx - 1].slots[0]);
            }
            level_num = i;
            idx = s_idx;
            slot_idx = j;
          }
          continue;
        }

        if (s_e.partial() != partial ||
            !key_equal{}(s_e.addr()->first, key))
          continue;

        if (!fix_dup) {
          result = FOUND_IN_RIGHT;
          level_num = i;
          idx = s_idx;
          slot_idx = j;

          return result;
        }

        if (result == FOUND_IN_LEFT || result == FOUND_IN_RIGHT) {
          // Refer to the same location
          if (s_e == prev_e) {
            // Duplication due to the re-insertion in normal
            // executions or os scheduling during a rehashing
            // operation. In this case, delete the pointer in
            // bottom level.
            if (prev_i < i) {
              del_dup(&s_b.slots[j],
                      &(levels[level_num]->buckets[idx].slots[slot_idx]), s_e,
                      prev_e);
            } else {
              // Never fires!
              assert(false);
            }
          }
          // Refer to different locations
          else {
            // Duplication due to the re-insertion after a crash
            // or concurrent insertions of same key. To fix the
            // duplication, simply delete the previous item.
            del_dup(&s_b.slots[j],
                    &(levels[level_num]->buckets[idx].slots[slot_idx]), s_e,
                    prev_e);
          }
          goto RETRY_FIND;
        } else {
          result = FOUND_IN_RIGHT;
          old_e = s_e;
          *e = &(s_b.slots[j]);
          level_num = i;
          idx = s_idx;
          slot_idx = j;
          if (last_bucket_entry != nullptr && s_idx != 0) {
            *last_bucket_entry = &(cl->buckets[s_idx - 1].slots[0]);
          }

          prev_e = s_e;
          prev_i = i;
        } // end if result in FOUND_IN_LEFT or FOUND_IN_RIGHT
      } // end for j, s_idx, s_b
    } // end for i, n_levels; end for first round

    // Context checking.
    level_meta *tmp_meta;
#ifdef OPT_CLEVEL_ROOT_READ
    tmp_meta = help_update->load_ptr(thread_id);
#else
    tmp_meta = meta.load();
#endif
    if (m_copy == tmp_meta) {
      return result;
    } else {
      m_copy = tmp_meta;
    }
  } // end while
}

template <typename Key, typename T, typename Hash, typename KeyEqual,
          size_t HashPower>
typename clevel_hash<Key, T, Hash, KeyEqual, HashPower>::ret
clevel_hash<Key, T, Hash, KeyEqual, HashPower>::generic_insert(
    const key_type &key, const void *param,
    void (*allocate_KV)(value_type *&, const void *), size_type thread_id,
    size_type id) {
  hv_type hv = hasher{}(key);
  partial_t partial = get_partial(hv);

  difference_type t_id = static_cast<difference_type>(thread_id);
  allocate_KV(tmp_entry[t_id], param);
  KV_entry_ptr_s created(tmp_entry[t_id]);
  created.set_partial(partial);

  bool expanded_flag = false;
  uint64_t initial_capacity = 0;
  bool check_duplicate = true;

#ifdef CLEVEL_DEBUG
  uint64_t retry_insert_cnt = 0;
  thread_logs[thread_id] << "Thread-" << thread_id << " starts inserting "
                         << key << std::endl;
#endif

#ifdef DEBUG_RESIZING
  initial_capacity = capacity();
#endif

  while (true) {
  RETRY_INSERT:
#ifdef CLEVEL_DEBUG
    retry_insert_cnt++;
    if (retry_insert_cnt > 10) {
      thread_logs[thread_id]
          << "Thread-" << thread_id
          << " [loop] retry_insert_cnt = " << retry_insert_cnt
          << ", key = " << key << std::endl;
    } else if (retry_insert_cnt > 1) {
      thread_logs[thread_id] << "Thread-" << thread_id
                             << " retry_insert_cnt = " << retry_insert_cnt
                             << ", key = " << key << std::endl;
    }
#endif
    // Assume we have limited area of cache coherence and we put meta in
    level_meta *m;
#ifdef OPT_CLEVEL_ROOT_READ
    m = help_update->load_ptr(thread_id);
#else
    m = meta.load();
#endif

    size_type n_levels;
    uint64_t level_num = 0;
    difference_type idx;
    KV_entry_ptr_s old_e;
    KV_entry_ptr_u *e;
    f_code_t result;
    if (check_duplicate) {
      result = find(key, partial, n_levels, old_e, &e, level_num, idx,
                    /*fix_dup=*/false, thread_id, m);
    } else {
      result = find_empty_slot(key, partial, n_levels, &e, level_num, m);
    }

    if (result == FOUND_IN_LEFT || result == FOUND_IN_RIGHT) {
      destroy_shared_value(tmp_entry[t_id]);
      return ret(level_num, 0, 0);
    } else if ((result == VACANCY_IN_LEFT || result == VACANCY_IN_RIGHT) &&
               (level_num > 0 || !m->is_resizing)) {
      uint64_t expected = old_e.p;
#ifdef NO_CC
      clwb(created.addr(), sizeof(created));
#endif
      if (e->p.compare_exchange_strong(expected, created.p)) {
#ifdef OPT_CLEVEL_ROOT_READ
        if (!m->is_resizing && help_update->load_ptr(thread_id)->is_resizing &&
            level_num == 0) {
#else
        if (!m->is_resizing && meta.load()->is_resizing && level_num == 0) {
#endif
          // Resizing may occur during the insert. Hence, redo the
          // insertion to avoid missing the new item. The possible
          // duplication will be fixed in future updates and
          // deletes.
          check_duplicate = false;
          goto RETRY_INSERT;
        } else {
          return ret(expanded_flag, initial_capacity);
        }

      } else {
#ifdef CLEVEL_DEBUG
        std::cout << "insertion, cas fails, n_levels: " << n_levels
                  << std::endl;
#endif
        goto RETRY_INSERT;
      }
    }

    // start expanding
    expanded_flag = true;
    expand(thread_id, m);
  } // end while(true)
}

template <typename Key, typename T, typename Hash, typename KeyEqual,
          size_t HashPower>
typename clevel_hash<Key, T, Hash, KeyEqual, HashPower>::ret
clevel_hash<Key, T, Hash, KeyEqual, HashPower>::erase(const key_type &key,
                                                      size_type thread_id) {
  hv_type hv = hasher{}(key);
  partial_t partial = get_partial(hv);
  bool succ_deletion = false;

#ifdef OPT_CLEVEL_ROOT_READ
  level_meta *m = help_update->load_ptr(thread_id);
#else
  level_meta *m = meta.load();
#endif
  while (true) {
    difference_type f_idx, s_idx;
    size_type i = 0;
    level_ptr_t li = nullptr, next_li = m->last_level;
    do {
      li = next_li;
      level_bucket *cl = li;
      f_idx = first_index(hv, cl->capacity);
      s_idx = second_index(partial, f_idx, cl->capacity);

      bucket &f_b = cl->buckets[f_idx];
      bucket &s_b = cl->buckets[s_idx];
      for (size_type j = 0; j < assoc_num; j++) {
        KV_entry_ptr_s tmp(f_b.slots[j]);
        if (tmp.partial(true) == partial && tmp.addr(true) != 0) {
          if (key_equal{}(tmp.addr(true)->first, key)) {
            uint64_t expected = tmp.p;
            if (f_b.slots[j].p.compare_exchange_strong(expected, 0)) {
              succ_deletion = true;

              destroy_shared_value(tmp.addr(true));

              // Instead of redoing the delete to
              // guarantee the deletion is successful,
              // we apply context checking to avoid
              // unnecessary re-executions. The deletion
              // fails only when the item to be deleted
              // is copied by rehashing threads after
              // checking and before deletion's CAS.
              // Therefore, we can do context checking
              // to avoid such failures.
#ifdef OPT_CLEVEL_ROOT_READ
              level_meta *tmp_meta{help_update->load_ptr(thread_id)};
#else
              level_meta *tmp_meta{meta.load()};
#endif
              if (tmp_meta != m)
                continue;

              if (i == 0) {
                if (f_idx == 0 && tmp_meta->is_resizing)
                  continue;

                KV_entry_ptr_s last_bucket_entry{
                    cl->buckets[f_idx - 1].slots[0]};
                if (last_bucket_entry.p == -1)
                  continue;
              }
            } else {
              continue;
            }
          }
        }
      }

      for (size_type j = 0; j < assoc_num; j++) {
        KV_entry_ptr_s tmp(s_b.slots[j]);
        if (tmp.partial(true) == partial && tmp.addr(true) != nullptr) {
          if (key_equal{}(tmp.addr(true)->first, key)) {
            uint64_t expected = tmp.p;
            bool ret = s_b.slots[j].p.compare_exchange_strong(expected, 0);
            if (ret) {
              succ_deletion = true;
              destroy_shared_value(tmp.addr(true));
              // Instead of redoing the delete to
              // guarantee the deletion is
              // successful, we apply context checking
              // to avoid unnecessary re-executions. The
              // deletion fails only when the item to be
              // deleted is copied by rehashing threads
              // after checking and before deletion's
              // CAS. Therefore, we can do context
              // checking to avoid such failures.
#ifdef OPT_CLEVEL_ROOT_READ
              level_meta *tmp_meta{help_update->load_ptr(thread_id)};
#else
              level_meta *tmp_meta{meta.load()};
#endif
              if (tmp_meta != m)
                continue;

              if (i == 0) {
                if (s_idx == 0 && tmp_meta->is_resizing)
                  continue;

                KV_entry_ptr_s last_bucket_entry{
                    cl->buckets[s_idx - 1].slots[0]};
                if (last_bucket_entry.p == -1)
                  continue;
              }
            } else {
              continue;
            }
          }
        }
      }
      next_li = cl->up;
      i++;
    } while (li != m->first_level);

    // Context checking.
    level_meta *tmp_meta;
#ifdef OPT_CLEVEL_ROOT_READ
    tmp_meta = help_update->load_ptr(thread_id);
#else
    tmp_meta = meta.load();
#endif
    if (tmp_meta == m) {
      return ret(succ_deletion);
    } else {
      m = tmp_meta;
    }
  } // end while(true)
}

template <typename Key, typename T, typename Hash, typename KeyEqual,
          size_t HashPower>
typename clevel_hash<Key, T, Hash, KeyEqual, HashPower>::ret
clevel_hash<Key, T, Hash, KeyEqual, HashPower>::generic_update(
    const key_type &key, const void *param,
    void (*allocate_KV)(value_type *&, const void *), size_type thread_id) {
  hv_type hv = hasher{}(key);
  partial_t partial = get_partial(hv);

  difference_type t_id = static_cast<difference_type>(thread_id);
  allocate_KV(tmp_entry[t_id], param);
  KV_entry_ptr_s created(tmp_entry[t_id]);
  created.set_partial(partial);

#ifdef NO_CC
  clwb(created.addr(), sizeof(created));
#endif

  bool succ_update = false;
  level_meta *m_copy;
#ifdef OPT_CLEVEL_ROOT_READ
  m_copy = help_update->load_ptr(thread_id);
#else
  m_copy = meta.load();
#endif

  while (true) {
    size_type n_levels;
    uint64_t level_num = 0;
    difference_type idx;
    KV_entry_ptr_s old_e;
    KV_entry_ptr_u *e, *last_bucket_entry = nullptr;

    f_code_t result =
        find(key, partial, n_levels, old_e, &e, level_num, idx,
             /*fix_dup=*/true, thread_id, m_copy, &last_bucket_entry);

    if (result == FOUND_IN_LEFT || result == FOUND_IN_RIGHT) {
      uint64_t expected;
      if (succ_update && old_e.p == created.p) {
        // The only item in table after update is the modified one,
        // which indicates a successful update.
        return ret(true);
      }

      else if (expected = old_e.p,
               e->p.compare_exchange_strong(expected, created.p)) {
        // Instead of simply issuing another find to guarantee
        // the update is successful, we apply context checking
        // to avoid unnecessary second find. The update fails
        // only when the item to be updated is copied by
        // rehashing threads after find and before update's
        // CAS. Therefore, we can do context checking to avoid
        // such failure.
        level_meta *tmp_meta;
#ifdef OPT_CLEVEL_ROOT_READ
        m_copy = help_update->load_ptr(thread_id);
#else
        tmp_meta = meta.load();
#endif
        if (tmp_meta != m_copy ||
            (level_num == 0 &&
             ((last_bucket_entry != nullptr && last_bucket_entry->p == -1) ||
              (last_bucket_entry == nullptr && tmp_meta->is_resizing)))) {
          succ_update = true;
          m_copy = tmp_meta;
          continue;
        }
        return ret(true);
      }
    } else {
      if (!succ_update) {
        destroy_shared_value(tmp_entry[t_id]);
      }
      // Even the updated item is deleted by other threads, our update
      // succeeds anyway.
      return ret(succ_update);
    }
  }
}

template <typename Key, typename T, typename Hash, typename KeyEqual,
          size_t HashPower>
void clevel_hash<Key, T, Hash, KeyEqual, HashPower>::expand(
    size_type thread_id, level_meta *m_copy) {
  difference_type t_id = static_cast<difference_type>(thread_id);
  level_bucket *cl = m_copy->first_level;

  if (cl->up.load() == nullptr) {
#ifdef USE_CXL
    tmp_level[t_id] = new (cacheable.malloc(sizeof(struct level_bucket))) level_bucket();
    if (tmp_level[t_id] == nullptr) {
      printf("Thread-%lu: Failed to allocate memory for level_bucket\n", thread_id);
      exit(1);
    }
#else
    tmp_level[t_id] = new level_bucket();
#endif
    size_type new_capacity = cl->capacity * 2;
#ifdef CLEVEL_DEBUG
    std::cout << "Thread-" << thread_id << " starts expanding for "
              << new_capacity << " buckets" << std::endl;
#endif

    /* delay malloc until the CAS succeeds */
    tmp_level[t_id]->allocate(new_capacity);
    tmp_level[t_id]->capacity = new_capacity;
    tmp_level[t_id]->up = nullptr;

    // Append a new level.
    level_bucket *expected = nullptr;
#ifdef NO_CC
    clwb(tmp_level[t_id], sizeof(level_bucket));
#endif
    bool rc = cl->up.compare_exchange_strong(expected, tmp_level[t_id]);

    if (rc == false) {
      // Ohter threads finished expanding
      tmp_level[t_id]->clear();
#ifdef USE_CXL
      cacheable.free(tmp_level[t_id]);
#else
      delete tmp_level[t_id];
#endif
    }

    // Update the first_level and is_resizing in the metadata.
    while (true) {
      if (cl->capacity >= new_capacity) {
        // Help updating meta
#ifdef USE_CXL
        tmp_meta[t_id] =
            new (cacheable.malloc(sizeof(level_meta))) level_meta(m_copy->first_level, m_copy->last_level, true);
#else
        tmp_meta[t_id] = new level_meta(m_copy->first_level, m_copy->last_level, true);
#endif
      } else {
#ifdef USE_CXL
        assert(cl->up.load() != nullptr);
        tmp_meta[t_id] =
            new (cacheable.malloc(sizeof(level_meta))) level_meta(cl->up.load(), m_copy->last_level, true);
#else
        tmp_meta[t_id] = new level_meta(cl->up.load(), m_copy->last_level, true);
#endif
      }

#ifdef NO_CC
      clwb(tmp_meta[t_id], sizeof(level_meta));
#endif

#ifdef OPT_CLEVEL_ROOT_READ
      if (help_update->cas_ptr(m_copy, tmp_meta[t_id])) {
#else
      if (meta.compare_exchange_strong(m_copy, tmp_meta[t_id])) {
#endif
// #ifdef CLEVEL_DEBUG
        std::cout << "Thread-" << thread_id
                  << " finishes expanding, capacity: " << capacity()
                  << std::endl;
// #endif
        break;
      } else {
#ifdef OPT_CLEVEL_ROOT_READ
        m_copy = help_update->load_ptr(t_id);
#else
        m_copy = meta.load();
#endif
        cl = m_copy->first_level;

        if (cl->capacity >= new_capacity && m_copy->is_resizing) {
          // CAS fails because other threads help updating
          // meta
#ifdef USE_CXL
          cacheable.free(tmp_meta[t_id]);
#else
          delete tmp_meta[t_id];
#endif
          break;
        }
        // CAS fails because other threads complete rehashing.
      }
    }
  } else {
    // Ohter threads finished expanding
#ifdef OPT_CLEVEL_ROOT_READ
      if (help_update->load_ptr(thread_id) == m_copy) {
#else
      if (meta.load() == m_copy) {
#endif
      size_type new_capacity = cl->capacity;

      // Update the first_level and is_resizing in the metadata.
      while (true) {
        // Help updating meta
        if (cl->capacity >= new_capacity) {
#ifdef USE_CXL  
          tmp_meta[t_id] =
              new (cacheable.malloc(sizeof(level_meta))) level_meta(m_copy->first_level, m_copy->last_level, true);
#else
        tmp_meta[t_id] = new level_meta(m_copy->first_level, m_copy->last_level, true);
#endif
        } else {
          assert(cl->up != nullptr);
#ifdef USE_CXL
          tmp_meta[t_id] = new (cacheable.malloc(sizeof(level_meta))) level_meta(cl->up, m_copy->last_level, true);
#else
          tmp_meta[t_id] = new level_meta(cl->up, m_copy->last_level, true);
#endif
        }
#ifdef NO_CC
    clwb(tmp_meta[t_id], sizeof(level_meta));
#endif
#ifdef OPT_CLEVEL_ROOT_READ
        if (help_update->cas_ptr(m_copy, tmp_meta[t_id])) {
#else
        if (meta.compare_exchange_strong(m_copy, tmp_meta[t_id])) {
#endif
#ifdef CLEVEL_DEBUG
          std::cout << "Thread-" << thread_id
                    << " finishes expanding, capacity: " << capacity()
                    << std::endl;
#endif
          break;
        } else {
#ifdef OPT_CLEVEL_ROOT_READ
          m_copy = help_update->load_ptr(thread_id);
#else
          m_copy = meta.load();
#endif
          cl = m_copy->first_level;

          if (cl->capacity >= new_capacity && m_copy->is_resizing) {
            // CAS fails because other threads help
            // updating meta
#ifdef USE_CXL
            cacheable.free(tmp_meta[t_id]);
#else
            delete tmp_meta[t_id];
#endif
            break;
          }
          // CAS fails because other threads complete
          // rehashing.
        }
      }
    }
  }
}

template <typename Key, typename T, typename Hash, typename KeyEqual,
          size_t HashPower>
void clevel_hash<Key, T, Hash, KeyEqual, HashPower>::resize() {
  size_type thread_id = SimThreadInfo::worker_thread_count - 1;
  difference_type t_id = static_cast<difference_type>(thread_id);
  difference_type expand_bucket = 0;

  while (run_expand_thread.load()) {
#ifdef OPT_CLEVEL_ROOT_READ
    level_meta *m = (help_update != nullptr) ? help_update->load_ptr(SimThreadInfo::worker_thread_id) : meta.load();
#else
    level_meta *m = meta.load();
#endif

    size_type n_levels = 1;
    if (m != nullptr) {
      for (level_bucket *li = m->last_level; li != m->first_level;
           li = li->up.load())
        n_levels++;
    }

    if (m == nullptr || n_levels == 2) {
      usleep(10000);
      continue;
    }

    for (size_type ii = 0; ii < resize_bulk; ii++) {
    RETRY_REHASH:
#ifdef OPT_CLEVEL_ROOT_READ
      m = (help_update != nullptr) ? help_update->load_ptr(SimThreadInfo::worker_thread_id) : meta.load();
#else
      m = meta.load();
#endif

      level_bucket *bl = m->last_level;
      level_bucket *tl = m->first_level;

      bucket &b = bl->buckets[expand_bucket];
      for (size_type slot_idx = 0; slot_idx < assoc_num; slot_idx++) {
        KV_entry_ptr_s src_tmp = b.slots[slot_idx];

        value_type *e = src_tmp.addr();
        if (e == nullptr)
          continue;

        difference_type f_idx, s_idx;
        bool succ = false;
        hv_type hv = hasher{}(e->first);
        partial_t partial = get_partial(hv);
        f_idx = first_index(hv, tl->capacity);
        s_idx = second_index(partial, f_idx, tl->capacity);

        bucket &dst_b1 = tl->buckets[f_idx];
        bucket &dst_b2 = tl->buckets[s_idx];
        for (size_type j = 0; j < assoc_num; j++) {
          // The rehashed item is inserted into the less-loaded
          // bucket between the two candidata buckets in the new
          // level.
          KV_entry_ptr_s dst_tmp = dst_b1.slots[j];
          if (dst_tmp.addr(true) == nullptr) {
            uint64_t expected = dst_tmp.p;
            if (dst_b1.slots[j].p.compare_exchange_strong(expected,
                                                          src_tmp.p)) {
              b.slots[slot_idx].p = 0;
              succ = true;
              break;
            }
          }

          dst_tmp = dst_b2.slots[j];
          if (dst_tmp.addr() == nullptr) {
            uint64_t expected = dst_tmp.p;
            if (dst_b2.slots[j].p.compare_exchange_strong(expected,
                                                          src_tmp.p)) {
              b.slots[slot_idx].p = 0;
              succ = true;
              break;
            }
          }
        } // end for

        if (!succ) {
          // std::cout << "expand during resizing!" << std::endl;
          expand(thread_id, m);
          goto RETRY_REHASH;
        }
      } // end for (slot_idx)

      expand_bucket++;
      if (static_cast<size_type>(expand_bucket) == bl->capacity) {
        bool rc = false;
        while (true) {
          level_ptr_t li = m->last_level;
          size_t levels_left = 0;
          while (li != m->first_level) {
            levels_left++;
            li = li->up;
          }
#ifdef USE_CXL
          tmp_meta[t_id] = new (cacheable.malloc(sizeof(level_meta))) level_meta(m->first_level, bl->up, levels_left != 2);
#else
          tmp_meta[t_id] = new level_meta(m->first_level, bl->up, levels_left != 2);
#endif

#ifdef NO_CC
    clwb(tmp_meta[t_id], sizeof(level_meta));
#endif

#ifdef OPT_CLEVEL_ROOT_READ
          if (help_update != nullptr && help_update->cas_ptr(m, tmp_meta[t_id])) {
#else
          if (meta.compare_exchange_strong(m, tmp_meta[t_id])) {
#endif
#ifdef CLEVEL_DEBUG
            std::cout << "Expand thread updates "
                         "metadata, "
                      << "is_resizing: " << bool(levels_left != 2)
                      << " levels_left: " << levels_left << std::endl;
#endif
            rc = true;
            expand_bucket = 0;
            break;
          } else {
#ifdef USE_CXL
            cacheable.free(tmp_meta[t_id]);
#else
            delete tmp_meta[t_id];
#endif
#ifdef OPT_CLEVEL_ROOT_READ
            m = help_update->load_ptr(SimThreadInfo::worker_thread_id);
#else
            m = meta.load();
#endif
          }
        }

        if (rc)
          break;
      }
    } // end for (ii)
  } // end while(run_expand_thread)

  std::cout << "expand_thread exits" << std::endl;
}

template <typename Key, typename T, typename Hash, typename KeyEqual,
          size_t HashPower>
typename clevel_hash<Key, T, Hash, KeyEqual, HashPower>::KV_entry_ptr_u &
clevel_hash<Key, T, Hash, KeyEqual, HashPower>::get_entry(level_ptr_t level,
                                                          difference_type idx,
                                                          uint64_t slot_idx) {
  return level->buckets[idx].slots[slot_idx];
}

template <typename Key, typename T, typename Hash, typename KeyEqual,
          size_t HashPower>
typename clevel_hash<Key, T, Hash, KeyEqual, HashPower>::key_type
clevel_hash<Key, T, Hash, KeyEqual, HashPower>::get_key(KV_entry_ptr_u &e) {
  return e.addr()->first;
}

template <typename Key, typename T, typename Hash, typename KeyEqual,
          size_t HashPower>
typename clevel_hash<Key, T, Hash, KeyEqual, HashPower>::key_type
clevel_hash<Key, T, Hash, KeyEqual, HashPower>::get_key(KV_entry_ptr_s &e) {
  return e.addr()->first;
}

template <typename Key, typename T, typename Hash, typename KeyEqual,
          size_t HashPower>
void clevel_hash<Key, T, Hash, KeyEqual, HashPower>::clear() {
  std::cout << "level destroy!" << std::endl;
}
