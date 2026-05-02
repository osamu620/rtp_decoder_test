#include "rtp_receiver.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>

namespace rtp {

namespace {
constexpr uint8_t kRtpVersion = 2;

inline uint16_t rd_u16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t rd_u32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
}  // namespace

Receiver::Receiver() {
  ring_ = std::unique_ptr<Slot[]>(new Slot[kRingSize]);
  slab_.assign(kRingSize * kSlotBytes, 0);
  job_queue_.assign(kJobQueueSize, Job{});
}

Receiver::~Receiver() { stop(); }

bool Receiver::start(const std::string& local_addr, uint16_t local_port, void* hook_arg, Hook hook) {
  if (running_.load()) return false;

  sock_fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock_fd_ < 0) {
    std::cerr << "rtp::Receiver: socket() failed: " << std::strerror(errno) << std::endl;
    return false;
  }

  int reuse = 1;
  ::setsockopt(sock_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  if (rcvbuf_size_ > 0) {
    ::setsockopt(sock_fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size_, sizeof(rcvbuf_size_));
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(local_port);
  if (local_addr.empty() || local_addr == "0.0.0.0" || local_addr == "*") {
    addr.sin_addr.s_addr = INADDR_ANY;
  } else if (::inet_pton(AF_INET, local_addr.c_str(), &addr.sin_addr) != 1) {
    std::cerr << "rtp::Receiver: inet_pton(" << local_addr << ") failed" << std::endl;
    ::close(sock_fd_);
    sock_fd_ = -1;
    return false;
  }

  if (::bind(sock_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::cerr << "rtp::Receiver: bind() failed: " << std::strerror(errno) << std::endl;
    ::close(sock_fd_);
    sock_fd_ = -1;
    return false;
  }

  hook_     = std::move(hook);
  hook_arg_ = hook_arg;
  started_  = false;
  next_seq_ = 0;
  pending_  = 0;
  for (size_t i = 0; i < kRingSize; ++i) {
    ring_[i].filled = false;
    ring_[i].len    = 0;
    ring_[i].in_worker.store(0, std::memory_order_relaxed);
  }
  job_head_.store(0, std::memory_order_relaxed);
  job_tail_.store(0, std::memory_order_relaxed);
  net_lost_packets_.store(0, std::memory_order_relaxed);
  slot_busy_drops_.store(0, std::memory_order_relaxed);
  queue_full_drops_.store(0, std::memory_order_relaxed);

  running_.store(true, std::memory_order_release);
  worker_ = std::thread([this] { worker_loop(); });
  thread_ = std::thread([this] { recv_loop(); });
  return true;
}

void Receiver::stop() {
  bool was_running = running_.exchange(false);
  if (sock_fd_ >= 0) {
    ::shutdown(sock_fd_, SHUT_RD);
    ::close(sock_fd_);
    sock_fd_ = -1;
  }
  if (was_running) {
    if (thread_.joinable()) thread_.join();
    {
      std::lock_guard<std::mutex> lk(worker_mu_);
    }
    worker_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }
}

void Receiver::recv_loop() {
  uint8_t buf[kSlotBytes];
  while (running_.load(std::memory_order_acquire)) {
    ssize_t n = ::recv(sock_fd_, buf, sizeof(buf), 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n < 12) continue;
    handle_dgram(buf, static_cast<size_t>(n));
  }
}

void Receiver::handle_dgram(uint8_t* data, size_t len) {
  uint8_t b0 = data[0];
  if ((b0 >> 6) != kRtpVersion) return;
  uint8_t cc  = b0 & 0x0F;
  uint8_t pad = (b0 >> 5) & 0x1;
  uint8_t ext = (b0 >> 4) & 0x1;

  size_t hdr = 12 + 4u * cc;
  if (len < hdr) return;
  if (ext) {
    if (len < hdr + 4) return;
    uint16_t ext_words = rd_u16(data + hdr + 2);
    hdr += 4u + 4u * ext_words;
    if (len < hdr) return;
  }

  size_t pad_len = 0;
  if (pad) {
    pad_len = data[len - 1];
    if (pad_len > len - hdr) return;
  }
  size_t effective_len = len - pad_len;
  if (effective_len > kSlotBytes) return;

  uint16_t seq = rd_u16(data + 2);

  if (!started_) {
    started_  = true;
    next_seq_ = seq;
  }

  int16_t diff = static_cast<int16_t>(seq - next_seq_);
  if (diff < 0) {
    return;  // late or duplicate
  }

  // If gap exceeds jitter depth, force-advance head past missing slots, dispatching any that did arrive.
  while (diff >= static_cast<int16_t>(jitter_depth_)) {
    size_t i   = next_seq_ & (kRingSize - 1);
    Slot& head = ring_[i];
    if (head.filled && head.seq == next_seq_) {
      dispatch(i, head.len, head.seq, head.hdr_len);
      head.filled = false;
      head.len    = 0;
      --pending_;
    } else {
      net_lost_packets_.fetch_add(1, std::memory_order_relaxed);
    }
    ++next_seq_;
    diff = static_cast<int16_t>(seq - next_seq_);
  }

  size_t idx = seq & (kRingSize - 1);
  Slot& slot = ring_[idx];
  if (slot.filled) {
    if (slot.seq == seq) return;  // duplicate, not a loss
    // Alias: ring slot already holds a different seq from a prior wrap-around.
    // The new packet was received but cannot be stored — count as net loss.
    net_lost_packets_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // Worker may still own this slab slot from an earlier in-flight job.
  if (slot.in_worker.load(std::memory_order_acquire) != 0) {
    slot_busy_drops_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  std::memcpy(slab_.data() + idx * kSlotBytes, data, effective_len);
  slot.seq     = seq;
  slot.hdr_len = static_cast<uint16_t>(hdr);
  slot.len     = effective_len;
  slot.filled  = true;
  ++pending_;

  release_in_order();
}

void Receiver::release_in_order() {
  while (pending_ > 0) {
    size_t i = next_seq_ & (kRingSize - 1);
    Slot& s  = ring_[i];
    if (!s.filled || s.seq != next_seq_) break;
    dispatch(i, s.len, s.seq, s.hdr_len);
    s.filled = false;
    s.len    = 0;
    --pending_;
    ++next_seq_;
  }
}

void Receiver::dispatch(size_t slab_idx, size_t len, uint16_t seq, uint16_t hdr_len) {
  // Hand off slab ownership to the worker, then enqueue the job.
  ring_[slab_idx].in_worker.store(1, std::memory_order_release);

  // Notify only on empty→non-empty transition. Saves one futex wakeup per packet at
  // steady state (worker is normally not in wait_for). The worker_loop's 1 ms wait_for
  // timeout is the safety net if a notify is missed due to a race with worker entering
  // wait between its empty-check and the actual sleep.
  const size_t head_before = job_head_.load(std::memory_order_acquire);
  const size_t tail_before = job_tail_.load(std::memory_order_relaxed);
  const bool was_empty     = (head_before == tail_before);

  if (!enqueue_job(Job{slab_idx, len, seq, hdr_len})) {
    // Worker is far behind. Drop this packet, return slab ownership to recv.
    ring_[slab_idx].in_worker.store(0, std::memory_order_release);
    queue_full_drops_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (was_empty) worker_cv_.notify_one();
}

bool Receiver::enqueue_job(const Job& j) {
  size_t tail      = job_tail_.load(std::memory_order_relaxed);
  size_t next_tail = (tail + 1) & (kJobQueueSize - 1);
  if (next_tail == job_head_.load(std::memory_order_acquire)) {
    return false;  // full
  }
  job_queue_[tail] = j;
  job_tail_.store(next_tail, std::memory_order_release);
  return true;
}

bool Receiver::dequeue_job(Job& out) {
  size_t head = job_head_.load(std::memory_order_relaxed);
  if (head == job_tail_.load(std::memory_order_acquire)) {
    return false;  // empty
  }
  out = job_queue_[head];
  job_head_.store((head + 1) & (kJobQueueSize - 1), std::memory_order_release);
  return true;
}

void Receiver::worker_loop() {
  while (running_.load(std::memory_order_acquire)) {
    Job j;
    if (dequeue_job(j)) {
      process_job(j);
      continue;
    }
    std::unique_lock<std::mutex> lk(worker_mu_);
    worker_cv_.wait_for(lk, std::chrono::milliseconds(1), [this] {
      return !running_.load(std::memory_order_acquire)
             || job_head_.load(std::memory_order_acquire)
                    != job_tail_.load(std::memory_order_acquire);
    });
  }
  // Drain remaining jobs so in-flight slot ownership flags don't leak.
  Job j;
  while (dequeue_job(j)) {
    process_job(j);
  }
}

void Receiver::process_job(const Job& j) {
  // hdr_len was parsed once in handle_dgram and carried through Slot+Job; no re-parse.
  const uint8_t* data = slab_.data() + j.slab_idx * kSlotBytes;
  if (j.hdr_len <= j.len && hook_) {
    const uint8_t b1 = data[1];
    Frame f{};
    f.seq          = j.seq;
    f.timestamp    = rd_u32(data + 4);
    f.ssrc         = rd_u32(data + 8);
    f.marker       = (b1 >> 7) & 0x1;
    f.payload_type = b1 & 0x7F;
    f.payload      = const_cast<uint8_t*>(data + j.hdr_len);
    f.payload_len  = j.len - j.hdr_len;
    hook_(hook_arg_, f);
  }
  ring_[j.slab_idx].in_worker.store(0, std::memory_order_release);
}

}  // namespace rtp
