#ifndef RTP_RECEIVER_HPP
#define RTP_RECEIVER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
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

  size_t lost_packets() const { return lost_packets_.load(std::memory_order_relaxed); }

  void set_jitter_depth(size_t depth) { jitter_depth_ = depth; }
  void set_recv_buf_size(int bytes) { rcvbuf_size_ = bytes; }

 private:
  static constexpr size_t kRingSize  = 1024;
  static constexpr size_t kSlotBytes = 9216;

  struct Slot {
    bool filled  = false;
    uint16_t seq = 0;
    size_t len   = 0;
  };

  void recv_loop();
  void handle_dgram(uint8_t* data, size_t len);
  void release_in_order();
  void dispatch(const uint8_t* data, size_t len, uint16_t seq);

  int sock_fd_ = -1;
  std::thread thread_;
  std::atomic<bool> running_{false};

  void* hook_arg_ = nullptr;
  Hook hook_;

  size_t jitter_depth_ = 64;
  int rcvbuf_size_     = 16 * 1024 * 1024;

  std::vector<Slot> ring_;
  std::vector<uint8_t> slab_;
  bool started_     = false;
  uint16_t next_seq_ = 0;
  size_t pending_    = 0;

  std::atomic<size_t> lost_packets_{0};
};

}  // namespace rtp

#endif  // RTP_RECEIVER_HPP
