#ifndef MSG_DISPATCHER_H
#define MSG_DISPATCHER_H
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64) || defined(__aarch64__)
#include <pthread.h>

#include <functional>

#include "connection/establish.h"
#include "msg_queue.h"

// Every type of message has a corresponding handler
using MsgHandler = std::function<void(msg_node_t *, int)>;

class MsgDispatcher {
 public:
  MsgDispatcher(msg_queue_t *q_addr, int from, msg_type_t type,
                MsgHandler handler);
  ~MsgDispatcher();

 private:
  MsgQueue queue;
  MsgHandler handler;
  pthread_t dispatchThread;
  bool stop;
  int from_;
  msg_type_t type;
  static void *dispatchLoop(void *arg);
};

extern MsgDispatcher *g_dispatcher[MSG_TYPE_NUM][NUM_CLIENTS];

#endif  // MSG_DISPATCHER_H
#endif
