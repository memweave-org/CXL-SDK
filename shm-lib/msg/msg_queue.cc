#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64) || defined(__aarch64__)
#include "msg/msg_queue.h"
#include <unistd.h>

#include <atomic>
#include <cstring>

#include "utils/bypass_cache.h"

MsgQueue::MsgQueue(void* q_addr) {
    queue = (msg_queue_t*)q_addr;
}

void MsgQueue::init_msg_queue() { queue->head = queue->tail = queue->msg; }

int MsgQueue::get_remain_space() const {
    if (queue->tail >= queue->head) {
        return sizeof(queue->msg) - (queue->tail - queue->head);
    } else {
        return queue->head - queue->tail;
    }
}

bool MsgQueue::is_queue_empty() const { return queue->head == queue->tail; }

int MsgQueue::enqueue_msg(msg_node_t* node_to_enqueue) {
    size_t total_size = align_to_cache_line(node_to_enqueue->length) ;

    size_t remaining_space = get_remain_space();
    msg_node_t* node;
    size_t space_at_end;
    char* tmp_tail = (char*)queue->tail;

    if (total_size > remaining_space)
        queue_full();

    node = (msg_node_t*)tmp_tail;
    space_at_end = sizeof(queue->msg) - (tmp_tail - queue->msg);

    if (space_at_end >= total_size) {
        // Enough space at the end of the queue
        memcpy_nt_write((char*)node, (char*)node_to_enqueue, total_size);
        tmp_tail = (char*)tmp_tail + total_size;
    } else {
        // Only msg_node can fit at the end of the queue
        size_t first_part_len = space_at_end;
        size_t second_part_len = total_size - first_part_len;
        memcpy_nt_write((char*)node, (char*)node_to_enqueue, first_part_len);
        memcpy_nt_write((char*)queue->msg,
                        (char*)node_to_enqueue + first_part_len,
                        second_part_len);
        tmp_tail = (char*)queue->msg + second_part_len;
    }

    // Set tail pointer to the start of the queue if it reaches the end
    adjust_tail(&tmp_tail);
    mfence();
    queue->tail = tmp_tail;
    return 0;
}

msg_node_t* MsgQueue::dequeue_msg(int blocking) {
    if (blocking) {
        while (is_queue_empty()) sleep(1);
    } else {
        if (is_queue_empty())
            return nullptr;
    }
    char* tmp_head = (char*)queue->head;
    msg_node_t* node = (msg_node_t*)tmp_head;
    size_t total_size = align_to_cache_line(node->length) ;
    msg_node_t* result = (msg_node_t*)malloc(total_size);

    size_t space_at_end = sizeof(queue->msg) - (tmp_head - queue->msg);

    if (space_at_end >= total_size) {
        // Enough space
        memcpy_nt_read((char*)result, (char*)node, total_size);
        tmp_head = (char*)tmp_head + total_size;
    } else {
        // Message spans like a ring
        size_t first_part_len = space_at_end;
        size_t second_part_len = total_size - first_part_len;
        memcpy_nt_read((char*)result, (char*)node, first_part_len);
        memcpy_nt_read((char*)result + first_part_len, queue->msg,
                       second_part_len);
        tmp_head = (char*)queue->msg + second_part_len;
    } 
    // Set head pointer to the start of the queue if it reaches the end
    adjust_head(&tmp_head);
    mfence();
    queue->head = tmp_head;
    return result;
}

// // Let the caller know the type of the message at the head of the queue
// void MsgQueue::peek_msg_type(msg_type_t* type, int* length) {
//     if (is_queue_empty()) {
//         *type = MSG_TYPE_NUM;  // MSG_TYPE_NUM means no message
//     } else {
//         volatile msg_node_t* volatile_node = (volatile msg_node_t*)queue->head;
//         *type = volatile_node->type;
//         *length = volatile_node->length;
//     }
// }

// Error handler
void MsgQueue::queue_full() {
    perror("Queue is full");
    // TODO: Expand the queue
    exit(EXIT_FAILURE);
}

// If tail pointer reaches the end of the queue, set it to the start
void MsgQueue::adjust_tail(char** tail) {
  uint64_t tail_offset = reinterpret_cast<uintptr_t>(*tail) -
                         reinterpret_cast<uintptr_t>(queue->msg);
  if (tail_offset == sizeof(queue->msg)) {
    *tail = (char*)queue->msg;
  } else if (tail_offset > sizeof(queue->msg)) {
    perror("Queue tail pointer out of range");
  }
}

// If head pointer reaches the end of the queue, set it to the start
void MsgQueue::adjust_head(char** head) {
  uint64_t head_offset = reinterpret_cast<uintptr_t>(*head) -
                         reinterpret_cast<uintptr_t>(queue->msg);
  if (head_offset == sizeof(queue->msg)) {
    *head = (char*)queue->msg;
  } else if (head_offset > sizeof(queue->msg)) {
    perror("Queue head pointer out of range");
  }
}
#endif
