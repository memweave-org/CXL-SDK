#pragma once

#include "clht_lb_res.h"

#include <atomic>
#include <vector>

class CLHTDB {
public:
    bool Read(uint64_t key, uint64_t& value);

    bool Update(uint64_t key, uint64_t value);

    bool Insert(uint64_t key, uint64_t value);

    bool Delete(uint64_t key);

    void Print();

    int PoolThreadInit();

    void PoolThreadClose(int thread_id);

    CLHTDB(int thread_num, int num_buckets);

    ~CLHTDB();

private:
    int allocate();
    void release(int id);

    clht_t* hashtable;
    int thread_num;
    std::vector<std::atomic<uint8_t>> bits;
};
