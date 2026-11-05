#pragma once
#ifdef YCSB_UB_TWO_HOST
#include "core/db.h"
namespace ycsbc {
// Only descriptors, transport, and genuinely shared index state live in NC.
DB* CreateUbDB(const std::string& name, int threads);
DB* LocalUbDB(DB* shared_descriptor);
int UbPrefillLocal(DB* local_db, uint64_t key, uintptr_t value);
void ReleaseLocalUbDBs();
// Fixed key partition: B owns even keys, A owns odd keys.
inline unsigned UbRpcOwner(uint64_t key) { return key & 1U; }
uint64_t UbRpcCompleted();
uint64_t UbRpcSent();
uint64_t UbRpcLocal();
const void* UbLocalIndexAddress(DB* shared_descriptor);
const char* UbRpcTransport();
}
#endif
