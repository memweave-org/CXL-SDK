#pragma once
#if !defined(YCSB_UB_TWO_HOST) || !defined(USE_NO_CC_QUEUE)
#error "UB read RPC requires the shared-memory MPMC queue"
#endif

#include "msg/mpmc_queue.h"
#include "shm/mm.h"
#include <stdexcept>
#include <thread>

namespace ycsbc {
// Placement-construct in NC, like the existing two-process RPC transport.
// Each client owns one request slot; only its integer ID crosses the queue.
class UbReadQueue {
  struct alignas(64) Message {
    uint64_t key=0, sequence=0;
    alignas(64) uintptr_t value=0;
    int result=0;
    nt<uint64_t> response_sequence{0};
  };
  MPMCQueue<uint32_t> requests_;
  Message* messages_;
  const int clients_;

 public:
  explicit UbReadQueue(int clients, size_t capacity=256)
      : requests_(capacity), clients_(clients) {
    if(clients<1 || clients>192) throw std::runtime_error("Invalid UB RPC client count");
    messages_=static_cast<Message*>(shared_atomic_memory().malloc(sizeof(Message)*clients));
    for(int i=0;i<clients;++i) new(messages_+i) Message;
  }
  UbReadQueue(const UbReadQueue&)=delete;
  UbReadQueue& operator=(const UbReadQueue&)=delete;
  ~UbReadQueue() {
    for(int i=0;i<clients_;++i) messages_[i].~Message();
    shared_atomic_memory().free(messages_);
    // The existing queue's slot storage belongs to the session's NC arena.
  }
  int read(int client,uint64_t key,uintptr_t& value) {
    if(client<0 || client>=clients_) throw std::runtime_error("Invalid UB RPC client slot");
    auto& message=messages_[client];
    message.key=key;
    const uint64_t sequence=++message.sequence;
    // Queue release/acquire publishes the NC request fields to the server.
    while(!requests_.try_enqueue(uint32_t(client))) std::this_thread::yield();
    while(message.response_sequence.load(std::memory_order_acquire)!=sequence)
      std::this_thread::yield();
    value=message.value;
    return message.result;
  }
  template<class Read> bool serve_one(Read&& read) {
    uint32_t client;
    if(!requests_.try_dequeue(client)) return false;
    if(client>=unsigned(clients_)) throw std::runtime_error("Invalid queued UB RPC client");
    auto& message=messages_[client];
    uintptr_t value=0;
    const int result=read(message.key,value);
    message.value=value;
    message.result=result;
    message.response_sequence.store(message.sequence,std::memory_order_release);
    return true;
  }
  // Placement diagnostics for the fixture contract, outside the request path.
  const void* request_storage() { return requests_.storage_address(); }
  const void* message_storage() const { return messages_; }
};
}
