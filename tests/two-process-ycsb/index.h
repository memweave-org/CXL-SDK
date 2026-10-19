#pragma once
#include <cstdint>
struct Handles { void* object{}; void* locks{}; void* roots{}; };
void index_open(Handles&, bool create, int threads);
void index_thread(int id);
void index_finish(int id);
void index_put(uint64_t key, uint64_t value);
void index_update(uint64_t key, uint64_t value);
bool index_get(uint64_t key, uint64_t& value);
void index_close();
