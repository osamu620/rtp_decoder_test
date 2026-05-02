#ifndef RTP_RECEIVER_HPP
#define RTP_RECEIVER_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rtp {

struct Frame {
  uint16_t seq;
  uint32_t timestamp;
  uint32_t ssrc;
  uint8_t marker;
  uint8_t payload_type;
  uint8_t* payload;
  size_t payload_len;
};

class Receiver {
 public:
  using Hook = std::function<void(void*, const Frame&)>;

  Receiver();
  ~Receiver();

  Receiver(const Receiver&)            = delete;
  Receiver& operator=(const Receiver&) = delete;

  bool start(const std::string& local_addr, uint16_t local_port, void* hook_arg, Hook hook);
  void stop();

  // True network loss: packets the network/sender never delivered (force-advance miss),
  // plus ring-alias drops where the slot was already occupied by a different sequence.
  size_t net_lost_packets() const { return net_lost_packets_.load(std::memory_order_relaxed); }
  // Backpressure drops: slab slot still owned by the worker thread (worker > kRingSize behind).
  size_t slot_busy_drops() const { return slot_busy_drops_.load(std::memory_order_relaxed); }
  // Backpressure drops: SPSC job queue full at dispatch (worker > kJobQueueSize behind).
  size_t queue_full_drops() const { return queue_full_drops_.load(std::memory_order_relaxed); }
  size_t total_drops() const { return net_lost_packets() + slot_busy_drops() + queue_full_drops(); }

  void set_jitter_depth(size_t depth) { jitter_depth_ = depth; }
  void set_recv_buf_size(int bytes) { rcvbuf_size_ = bytes; }

 private:
  static constexpr size_t kRingSize     = 1024;
  static constexpr size_t kSlotBytes    = 9216;
  static constexpr size_t kJobQueueSize = kRingSize;

  struct Slot {
    bool filled      = false;
    uint16_t seq     = 0;
    uint16_t hdr_len = 0;  // RTP header length (parsed once in handle_dgram)
    size_t len       = 0;
    std::atomic<uint8_t> in_worker{0};  // 0 = recv may write, 1 = worker holds slab bytes
  };

  struct Job {
    size_t slab_idx;
    size_t len;
    uint16_t seq;
    uint16_t hdr_len;
  };

  void recv_loop();
  void worker_loop();
  void handle_dgram(uint8_t* data, size_t len);
  void release_in_order();
  void dispatch(size_t slab_idx, size_t len, uint16_t seq, uint16_t hdr_len);
  void process_job(const Job& j);
  bool enqueue_job(const Job& j);
  bool dequeue_job(Job& out);

  int sock_fd_ = -1;
  std::thread thread_;
  std::thread worker_;
  std::atomic<bool> running_{false};

  void* hook_arg_ = nullptr;
  Hook hook_;

  size_t jitter_depth_ = 64;
  int rcvbuf_size_     = 16 * 1024 * 1024;

  std::unique_ptr<Slot[]> ring_;
  std::vector<uint8_t> slab_;
  bool started_      = false;
  uint16_t next_seq_ = 0;
  size_t pending_    = 0;

  std::atomic<size_t> net_lost_packets_{0};
  std::atomic<size_t> slot_busy_drops_{0};
  std::atomic<size_t> queue_full_drops_{0};

  // SPSC job queue: producer = recv thread, consumer = worker thread.
  std::vector<Job> job_queue_;
  alignas(64) std::atomic<size_t> job_head_{0};  // consumer index
  alignas(64) std::atomic<size_t> job_tail_{0};  // producer index
  std::mutex worker_mu_;
  std::condition_variable worker_cv_;
};

}  // namespace rtp

#endif  // RTP_RECEIVER_HPP
