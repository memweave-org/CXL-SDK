#pragma once
#ifdef YCSB_UB_TWO_HOST
#include "core/db.h"
namespace ycsbc {
// Objects and mailboxes live in NC. Threads and SDK handles remain process-local.
DB* CreateUbDB(const std::string& name, int threads);
uint64_t UbRpcCompleted();
}
#endif
