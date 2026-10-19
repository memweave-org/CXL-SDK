#include "index.h"
#include "Tree.h"
static Tree* tree;
static DSM* dsm;
void index_open(Handles& h,bool create,int threads) {
  if(!create) { global_lock_table=static_cast<nt<uint64_t>*>(h.locks); root_ptr_list=static_cast<nt<void*>*>(h.roots); }
  DSMConfig c; c.machineNR=1; c.cacheConfig.cacheSize=((uint64_t(threads)+1)*12+1023)/1024;
  dsm=DSM::getInstance(c); dsm->set_process_id(SimThreadInfo::worker_machine_id); dsm->registerThread(); tree=new Tree(dsm);
  if(create) { h.locks=global_lock_table; h.roots=root_ptr_list; }
}
void index_thread(int) { dsm->registerThread(); tree->register_thread(0); }
void index_finish(int) {}
void index_put(uint64_t k,uint64_t v) { tree->insert(k,v); }
bool index_get(uint64_t k,uint64_t& v) { return tree->search(k,v); }
void index_close() {}

void index_update(uint64_t k, uint64_t v) { index_put(k,v); }
