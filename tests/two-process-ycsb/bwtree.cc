#include "index.h"
#include "test_suite.h"
static TreeType* tree;
void index_open(Handles& h, bool create, int threads) {
  if (create) {
    tree = new (shared_atomic_memory().malloc(sizeof(TreeType))) TreeType(false, KeyComparator{1}, KeyEqualityChecker{1});
    tree->UpdateThreadLocal(threads + 1);
    h.object = tree;
  } else tree = static_cast<TreeType*>(h.object);
}
void index_thread(int id) { tree->AssignGCID(id); }
void index_finish(int id) { tree->UnregisterThread(id); }
void index_put(uint64_t k, uint64_t v) { if (!tree->Insert(k,v)) throw std::runtime_error("duplicate insert"); }
bool index_get(uint64_t k, uint64_t& v) {
  std::vector<long> values; tree->GetValue(k,values);
  if (values.size()!=1) return false;
  v=values[0]; return true;
}
void index_close() {}

void index_update(uint64_t k, uint64_t v) { index_put(k,v); }
