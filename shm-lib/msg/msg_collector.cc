#include "msg/msg_collector.h"
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64) || defined(__aarch64__)
MsgCollector* g_collector[MSG_TYPE_NUM][NUM_CLIENTS];

MsgCollector::MsgCollector(void* q_addr) : queue(q_addr) {
    // Initialize any necessary variables or data structures
}

MsgCollector::~MsgCollector() {}

void MsgCollector::sendMsg(msg_node_t* node) {
    q_lock.lock();
    queue.enqueue_msg(node);
    q_lock.unlock();
}
#endif
