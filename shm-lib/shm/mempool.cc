#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64) || defined(__aarch64__)
#include "shm/mempool.h"

#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <iostream>

#include "msg/msg_collector.h"
#include "msg/msg_dispatcher.h"
#include "utils/sim_id.h"

struct free_request {
    void* ptr;
};

void listen_free_request(msg_node_t* node, int from) {
    free_request* req = reinterpret_cast<free_request*>(node->content);
    cacheable.free(req->ptr);
    // std::cout << "free request received" << std::endl;
    free(node);
}

inline msg_queue_t* get_msg_queue(int src, int dst, msg_type_t type) {
    int actual_pos = dst > src ? dst - 1 : dst;
    return reinterpret_cast<msg_queue_t*>(
        reinterpret_cast<size_t>(QUEUE_BASE) + type * QUEUE_SIZE +
        (src * (NUM_CLIENTS - 1) + actual_pos) * sizeof(msg_queue_t));
}

int get_ptr_machine_index(void* ptr) {
    size_t ptr_t = reinterpret_cast<size_t>(ptr);
    size_t gb_t = reinterpret_cast<size_t>(GLOBAL_BASE);
    if (ptr_t >= gb_t && (ptr_t < gb_t + SHM_TOTAL_SIZE)) {
        return (ptr_t - gb_t) / SHM_SIZE;
    } else {
        return -1;
    }
}

void marker_free_cross_machine(memkind_t kind, void* ptr) {
    if (ptr < LOCAL_BASE || ptr >= LOCAL_BORDER) {
        int index = get_ptr_machine_index(ptr);
        if (index == -1) {
            perror("Invalid free request.");
            exit(EXIT_FAILURE);
        }
        msg_node_t* node = reinterpret_cast<msg_node_t*>(
            malloc(sizeof(msg_node_t) + sizeof(free_request)));
        node->type = FREE;
        node->length = sizeof(free_request);
        memcpy(node->content, &ptr, sizeof(void*));
        g_collector[FREE][index]->sendMsg(node);
        free(node);
    } else {
        memkind_free(kind, ptr);
    }
}

void init_msg_queue() {
  if (SimThreadInfo::worker_machine_id == 0) {
    for (int t = 0; t < MSG_TYPE_NUM; t++) {
      for (int i = 0; i < NUM_CLIENTS; i++) {
        for (int j = 0; j < NUM_CLIENTS; j++) {
          if (i == j) {
            continue;
          }
          MsgQueue q(get_msg_queue(i, j, static_cast<msg_type_t>(t)));
          q.init_msg_queue();
        }
      }
    }
  }
}

void initialize_shm_and_queue() {
    // initialize_shm();
    init_msg_queue();
}

void add_dispatcher(msg_type_t type, MsgHandler handler) {
  for (int i = 0; i < NUM_CLIENTS; i++) {
    if (i == SimThreadInfo::worker_machine_id) continue;
    msg_queue_t* queue_recv = get_msg_queue(i, SimThreadInfo::worker_machine_id, type);
    g_dispatcher[type][i] = new MsgDispatcher(queue_recv, i, type, handler);

    msg_queue_t* queue_send = get_msg_queue(SimThreadInfo::worker_machine_id, i, type);
    g_collector[type][i] = new MsgCollector(queue_send);
  }
}

void initialize_comm(int machine_no) {
    add_dispatcher(FREE, listen_free_request);
#ifdef CROSS_MACHINE_LOCK_DELE
    /* register handler for lock init */
    add_dispatcher(LOCK_DELE, listen_lock_dele_request);
#endif
}

#ifdef CROSS_MACHINE_LOCK_DELE
pthread_key_t CAS_notify;
void notify_destruct(void* ptr) {
    if (ptr >= LOCAL_BASE && ptr < LOCAL_BORDER) {
        memkind_free(memkind_pool, ptr);
    }
}
#endif
#endif

/* Memkind */
