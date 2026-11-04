#include "msg/msg_dispatcher.h"
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64) || defined(__aarch64__)
#include <cstdlib>

MsgDispatcher* g_dispatcher[MSG_TYPE_NUM][NUM_CLIENTS];

void* MsgDispatcher::dispatchLoop(void* arg) {
    auto dispatcher = (MsgDispatcher*)arg;
    while (!dispatcher->stop) {
        msg_node_t* node = dispatcher->queue.dequeue_msg(0);
        if (node == nullptr)
            continue;
        dispatcher->handler(node, dispatcher->from_);
    }
    return NULL;
}

MsgDispatcher::MsgDispatcher(msg_queue_t* q_addr, int from, msg_type_t type,
                             MsgHandler handler)
    : queue(q_addr), stop(false), from_(from), type(type), handler(handler) {
    pthread_create(&dispatchThread, NULL, MsgDispatcher::dispatchLoop, this);
}

MsgDispatcher::~MsgDispatcher() {
    stop = true;
    pthread_join(dispatchThread, NULL);
}
#endif
