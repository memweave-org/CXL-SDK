#include "index.h"
#include "shm/cxl_type.h"
#include "clevel_hash.hpp"
using Index = clevel_hash<uint64_t,uint64_t>;
static Index* tree;
void index_open(Handles& h, bool create, int threads) {
  if (create) {
    tree = new (cacheable.malloc(sizeof(Index))) Index();
    tree->set_thread_num(threads + 1); h.object=tree;
  } else tree=static_cast<Index*>(h.object);
}
void index_thread(int) {}
void index_finish(int) {}
void index_put(uint64_t k,uint64_t v) { tree->insert({k,v},SimThreadInfo::worker_thread_id,k); }
bool index_get(uint64_t k,uint64_t& v) { auto r=tree->search(k); if (!r.found) return false; v=r.value->second; return true; }
void index_close() {}

void index_update(uint64_t k, uint64_t v) { tree->update({k,v},SimThreadInfo::worker_thread_id); }
