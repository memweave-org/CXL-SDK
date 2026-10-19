#ifndef MSG_QUEUE_HH
#define MSG_QUEUE_HH
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64) || defined(__aarch64__)
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "utils/compiler.h"

typedef enum msg_type { FREE, LOCK_DELE, BWTREE_GC, MSG_TYPE_NUM } msg_type_t;

struct alignas(CACHE_LINE_SIZE) msg_node {
  size_t length;
  msg_type_t type;
  int padding;     // Padding to 8 bytes
  char content[];  // Variable length array
};

typedef struct msg_node msg_node_t;

#define QUEUE_PAGE_CNT (512)
struct alignas(4096) msg_queue {
  volatile char *head;
  volatile char *tail;
  char padding[CACHE_LINE_SIZE - 2 * sizeof(char *)];
  char msg[QUEUE_PAGE_CNT * 4096 - CACHE_LINE_SIZE];
};

typedef struct msg_queue msg_queue_t;

class MsgQueue {
 public:
  MsgQueue(void *q_addr);
  ~MsgQueue() = default;
  void init_msg_queue();
  int get_remain_space() const;
  bool is_queue_empty() const;
  int enqueue_msg(msg_node_t *node_to_enqueue);
  msg_node_t *dequeue_msg(int blocking);
  // void peek_msg_type(msg_type_t *type, int *length);

 private:
  msg_queue_t *queue;

  void queue_full();
  void adjust_tail(char **tail);
  void adjust_head(char **head);
  inline size_t align_to_cache_line(size_t size) {
    return (size + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
  }
};

#endif  // MSG_QUEUE_HH
#endif
