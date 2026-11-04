#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64) || defined(__aarch64__)
#ifndef MSG_COLLECTOR_HH
#define MSG_COLLECTOR_HH

#include <pthread.h>

#include <functional>
#include <mutex>

#include "connection/establish.h"
#include "msg_queue.h"

#define NUM_CLIENTS 2

class MsgCollector {
public:
  MsgCollector(void *q_addr);
  ~MsgCollector();
  void sendMsg(msg_node_t *node);

private:
  MsgQueue queue;
  std::mutex q_lock;
};

extern MsgCollector *g_collector[MSG_TYPE_NUM][NUM_CLIENTS];

#endif  // MSG_COLLECT_HH
#endif