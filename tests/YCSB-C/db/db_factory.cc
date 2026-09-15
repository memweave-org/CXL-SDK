//
//  basic_db.cc
//  YCSB-C
//
//  Created by Jinglei Ren on 12/17/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#include "db/db_factory.h"
#include "utils/helper.h"
#include "utils/sim_id.h"

#include <string>

// #define ENABLE_BTREE_DB
// #define ENABLE_BWTREE_DB
// #define ENABLE_BTREE_OLC_DB
// #define ENABLE_RADIX_ART_OLC_DB
// #define ENABLE_RADIX_ART_ROWEX_DB
// #define ENABLE_CLHT_DB
// #define ENABLE_HOT_DB
// #define ENABLE_MASSTREE_DB
// #define ENABLE_CLEVEL_HASH_DB

#ifdef ENABLE_BTREE_DB
#include "db/btree_db.h"
#endif
#ifdef ENABLE_BWTREE_DB
#include "db/bwtree_db.h"
#endif
#ifdef ENABLE_BTREE_OLC_DB
#include "db/btree_olc_db.h"
#endif
#ifdef ENABLE_RADIX_ART_OLC_DB
#include "db/radix_art_olc_db.h"
#endif
#ifdef ENABLE_RADIX_ART_ROWEX_DB
#include "db/radix_art_rowex_db.h"
#endif
#ifdef ENABLE_CLHT_DB
#include "db/clht_db.h"
#endif
#ifdef ENABLE_HOT_DB
#include "db/hot_db.h"
#endif
#ifdef ENABLE_MASSTREE_DB
#include "db/masstree_db.h"
#endif
#ifdef ENABLE_CLEVEL_HASH_DB
#include "db/clevel_hash_db.h"
#endif

using namespace std;
using ycsbc::DB;
using ycsbc::DBFactory;

DB *DBFactory::CreateDB(utils::Properties &props) {
#ifdef USE_MSG_QUEUE
  int num_db = SimThreadInfo::worker_db_count;
  int db_thread = SimThreadInfo::worker_thread_count / num_db + 1;
#else
  int db_thread = SimThreadInfo::worker_thread_count + 1;
#endif
#ifdef ENABLE_RADIX_ART_ROWEX_DB
  if (props["dbname"] == "radix_art_rowex") {
    return ALLOC_AND_CONSTRUCT(RadixARTROWEXDB, cacheable.malloc);
  } else
#endif
#ifdef ENABLE_RADIX_ART_OLC_DB
      if (props["dbname"] == "radix_art_olc") {
    return new RadixARTOLCDB(db_thread);
  } else
#endif
#ifdef ENABLE_BWTREE_DB
      if (props["dbname"] == "bwtree") {
    // return new BwTreeDB(db_thread);
    return ALLOC_AND_CONSTRUCT(BwTreeDB, cacheable.malloc, db_thread);
  } else
#endif
#ifdef ENABLE_BTREE_DB
      if (props["dbname"] == "btree") {
    return ALLOC_AND_CONSTRUCT(BTreeDB, cacheable.malloc, db_thread);
  } else
#endif
#ifdef ENABLE_BTREE_OLC_DB
      if (props["dbname"] == "btree_olc") {
    return ALLOC_AND_CONSTRUCT(BTreeOLCDB, cacheable.malloc, db_thread);
  } else
#endif
#ifdef ENABLE_CLHT_DB
      if (props["dbname"] == "clht") {
    return ALLOC_AND_CONSTRUCT(CLHTDB, cacheable.malloc, db_thread, 512);
  } else
#endif
#ifdef ENABLE_HOT_DB
      if (props["dbname"] == "hot") {
    return ALLOC_AND_CONSTRUCT(HotDB, cacheable.malloc, db_thread);
  } else
#endif
#ifdef ENABLE_MASSTREE_DB
      if (props["dbname"] == "masstree") {
    return ALLOC_AND_CONSTRUCT(MasstreeDB, cacheable.malloc, db_thread);
  } else
#endif
#ifdef ENABLE_CLEVEL_HASH_DB
      if (props["dbname"] == "clevelhash") {
    return ALLOC_AND_CONSTRUCT(CLevelHashDB, cacheable.malloc, db_thread);
  } else
#endif
  return NULL;
}
