#include "clht_db.h"

#include "clht_lb_res.h"

CLHTDB::CLHTDB(int thread_num, int num_buckets) : thread_num(thread_num), bits(8 * thread_num) {
    hashtable = clht_create(num_buckets);
    if (hashtable == nullptr) {
        throw std::runtime_error("Error: failed to create hashtable");
    }
}

CLHTDB::~CLHTDB() {}

bool CLHTDB::Read(uint64_t key, uint64_t& value) {
    static thread_local bool finish_gc_init = false;
    if (unlikely(!finish_gc_init)) {
        PoolThreadInit();
        finish_gc_init = true;
    }
    if (key == 0) {
        std::cerr << "Error: key is 0, read failed" << std::endl;
        return false;
    }
    value = clht_get(this->hashtable->data.fields.ht, key);
    return true;
}

bool CLHTDB::Update(uint64_t key, uint64_t value) {
    static thread_local bool finish_gc_init = false;
    if (unlikely(!finish_gc_init)) {
        PoolThreadInit();
        finish_gc_init = true;
    }
    if (key == 0) {
        std::cerr << "Error: key is 0, update failed" << std::endl;
        return false;
    }
    bool ret = clht_put(this->hashtable, key, value);
    return ret;
}

bool CLHTDB::Insert(uint64_t key, uint64_t value) {
    static thread_local bool finish_gc_init = false;
    if (unlikely(!finish_gc_init)) {
        PoolThreadInit();
        finish_gc_init = true;
    }
    if (key == 0) {
        std::cerr << "Error: key is 0, insert failed" << std::endl;
        return false;
    }
    bool ret = clht_put(this->hashtable, key, value);
    return ret;
}

bool CLHTDB::Delete(uint64_t key) {
    static thread_local bool finish_gc_init = false;
    if (unlikely(!finish_gc_init)) {
        PoolThreadInit();
        finish_gc_init = true;
    }
    if (key == 0) {
        std::cerr << "Error: key is 0, delete failed" << std::endl;
        return false;
    }
    bool ret = clht_remove(this->hashtable, key);
    return ret;
}

void CLHTDB::Print() {
    clht_print(this->hashtable->data.fields.ht);
}

int CLHTDB::PoolThreadInit() {
    int thread_id = allocate();
    clht_gc_thread_init(hashtable, thread_id);
    return thread_id;
}

void CLHTDB::PoolThreadClose(int thread_id) {
    release(thread_id);
}

int CLHTDB::allocate() {
    for (int i = 0; i < (int)bits.size(); ++i) {
        uint8_t expected = 0;
        if (bits[i].compare_exchange_strong(expected, 1)) {
            return i;
        }
    }
    return -1;
}

void CLHTDB::release(int id) {
    if (id >= 0 && id < (int)bits.size()) {
        bits[id].store(0);
    }
}