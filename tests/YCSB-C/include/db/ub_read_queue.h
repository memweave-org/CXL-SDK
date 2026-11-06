#pragma once
#if !defined(YCSB_UB_TWO_HOST) || !defined(USE_NO_CC_QUEUE)
#error "UB read RPC requires the shared-memory MPMC queue"
#endif

#include "msg/mpmc_queue.h"
#include "shm/mm.h"
#include <stdexcept>
#include <thread>
#include <vector>
#include <cstring>

namespace ycsbc {
// Placement-construct in NC, like the existing two-process RPC transport.
// Each client owns one request slot; only its integer ID crosses the queue.
class UbReadQueue {
public:
  enum class Operation : uint32_t { Read, Update, Insert };
private:
  struct alignas(64) Message {
    uint64_t key=0, sequence=0;
    Operation operation=Operation::Read;
    alignas(64) uintptr_t value=0;
    int result=0;
    nt<uint64_t> response_sequence{0};
  };
  MPMCQueue<uint32_t>* requests_;
  Message* messages_;
  const int clients_;
  const size_t value_bytes_, payload_stride_;
  char* payloads_=nullptr;

 public:
  explicit UbReadQueue(int clients, size_t capacity=256, size_t value_bytes=0)
      : clients_(clients), value_bytes_(value_bytes),
        payload_stride_((value_bytes+63)&~size_t(63)) {
    if(clients<1 || clients>192) throw std::runtime_error("Invalid UB RPC client count");
    requests_=static_cast<MPMCQueue<uint32_t>*>(shared_atomic_memory().malloc(sizeof(*requests_)*clients));
    for(int i=0;i<clients;++i) new(requests_+i) MPMCQueue<uint32_t>(capacity);
    messages_=static_cast<Message*>(shared_atomic_memory().malloc(sizeof(Message)*clients));
    for(int i=0;i<clients;++i) new(messages_+i) Message;
    if(value_bytes_) payloads_=static_cast<char*>(shared_atomic_memory().malloc(payload_stride_*clients));
  }
  UbReadQueue(const UbReadQueue&)=delete;
  UbReadQueue& operator=(const UbReadQueue&)=delete;
  ~UbReadQueue() {
    for(int i=0;i<clients_;++i) messages_[i].~Message();
    shared_atomic_memory().free(messages_);
    for(int i=0;i<clients_;++i) requests_[i].~MPMCQueue<uint32_t>();
    shared_atomic_memory().free(requests_);
    if(payloads_) shared_atomic_memory().free(payloads_);
    // The existing queue's slot storage belongs to the session's NC arena.
  }
  int read(int client,uint64_t key,uintptr_t& value) {
    return request(client,Operation::Read,key,value);
  }
  int request(int client,Operation operation,uint64_t key,uintptr_t& value) {
    if(client<0 || client>=clients_) throw std::runtime_error("Invalid UB RPC client slot");
    auto& message=messages_[client];
    message.key=key;
    message.operation=operation;
    if(operation!=Operation::Read) {
      if(!value_bytes_) throw std::runtime_error("RPC writes require a value payload");
      std::memcpy(payloads_+client*payload_stride_,reinterpret_cast<const void*>(value),value_bytes_);
    }
    const uint64_t sequence=++message.sequence;
    // Queue release/acquire publishes the NC request fields to the server.
    while(!requests_[client].try_enqueue(uint32_t(client))) relax();
    while(message.response_sequence.load(std::memory_order_acquire)!=sequence)
      relax();
    if(operation==Operation::Read && value_bytes_ && message.result==0) {
      // Return a caller-private copy, never a pointer into the other process.
      thread_local std::vector<char> response;
      response.resize(value_bytes_);
      std::memcpy(response.data(),payloads_+client*payload_stride_,value_bytes_);
      value=reinterpret_cast<uintptr_t>(response.data());
    } else value=message.value;
    return message.result;
  }
  template<class Read> bool serve_one(Read&& read) {
    return serve_request([&](Operation operation,uint64_t key,uintptr_t& value) {
      if(operation!=Operation::Read) throw std::runtime_error("Read-only RPC handler received a mutation");
      return read(key,value);
    });
  }
  static void relax() {
#if defined(__x86_64__) || defined(__i386__)
    asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
  }
  size_t buckets() const { return clients_; }
  template<class Execute> bool serve_request(Execute&& execute) {
    for(int i=0;i<clients_;++i) if(serve_bucket(i,execute)) return true;
    return false;
  }
  template<class Execute> bool serve_bucket(size_t bucket, Execute&& execute) {
    if(bucket>=size_t(clients_)) return false;
    uint32_t client;
    if(!requests_[bucket].try_dequeue(client)) return false;
    if(client>=unsigned(clients_)) throw std::runtime_error("Invalid queued UB RPC client");
    auto& message=messages_[client];
    uintptr_t value=message.operation==Operation::Read ? 0 :
        reinterpret_cast<uintptr_t>(payloads_+client*payload_stride_);
    const int result=execute(message.operation,message.key,value);
    if(value_bytes_ && message.operation==Operation::Read && result==0)
      std::memcpy(payloads_+client*payload_stride_,reinterpret_cast<const void*>(value),value_bytes_);
    // Payload-mode replies must not expose the server's private address.
    message.value=value_bytes_ ? 0 : value;
    message.result=result;
    message.response_sequence.store(message.sequence,std::memory_order_release);
    return true;
  }
  // Placement diagnostics for the fixture contract, outside the request path.
  const void* request_storage() { return requests_[0].storage_address(); }
  const void* message_storage() const { return messages_; }
  const void* payload_storage() const { return payloads_; }
};
}
