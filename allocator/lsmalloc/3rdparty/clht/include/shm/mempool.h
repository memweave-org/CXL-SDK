#ifndef MEMPOOL_HH
#define MEMPOOL_HH
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include "memkind.h"
#include "msg/msg_dispatcher.h"
// #include "shm/lock/shm_lock_base.h"

#ifdef CROSS_MACHINE_LOCK_DELE
extern pthread_key_t CAS_notify;
void notify_destruct(void *ptr);
#endif

void initialize_shm_and_queue();
void initialize_comm(int machine_no);
void add_dispatcher(msg_type_t type, MsgHandler handler);

void marker_free_cross_machine(memkind_t kind, void* ptr);
int get_ptr_machine_index(void* ptr);

#endif
#endif
