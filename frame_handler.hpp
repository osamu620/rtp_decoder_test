//
// Created by OSAMU WATANABE on 2024/12/01.
//

#ifndef FRAME_HANDLER_HPP
#define FRAME_HANDLER_HPP
#include <cstring>
#include <chrono>
#include <vector>
#include <packet_parser/tile_handler.hpp>
#include <packet_parser/j2k_header.hpp>
#include <packet_parser/utils.hpp>

#if defined(__aarch64__)
  #include <arm_neon.h>
#endif

#define ACTION(func, ...)                                                         \
  auto st = std::chrono::high_resolution_clock::now();                            \
  if (tile_hndr.func(__VA_ARGS__)) {                                              \
    log_put("**************** FAILURE");                                          \
    is_parsing_failure = 1;                                                       \
    is_passed_header   = 0;                                                       \
  }                                                                               \
  auto dr    = std::chrono::high_resolution_clock::now() - st;                    \
  auto count = std::chrono::duration_cast<std::chrono::microseconds>(dr).count(); \
  cumlative_time += static_cast<double>(count)

namespace j2k {

// Per-packet hook into the receiver: called by the user's RTP hook to pass each
// arriving body packet's J2K bytes (chunk) to the parser. The slab the bytes live in
// is held until release_slab_cb_ is invoked at the next frame boundary.
class frame_handler {
 public:
  // Called by the receiver's hook to release a slab back when frame_handler is done
  // with all chunks of a frame. Wired up via set_release_slab_callback so frame_handler
  // doesn't need to know about rtp::Receiver directly.
  using ReleaseSlabCb = void (*)(void *user, size_t slab_idx);

 private:
  // Held slab indices for the in-flight frame. Drained at EOC via release_slab_cb_.
  std::vector<size_t> held_slabs_;
  // Running total of bytes appended to cs's chain — equal to "current end of chain
  // before the next append." Used to compute resync byte offsets and start_SOD.
  size_t chain_total_bytes_;
  size_t total_frames;
  size_t trunc_frames;
  size_t lost_frames;
  uint32_t start_SOD;
  int32_t is_parsing_failure;
  int32_t is_passed_header;
  std::chrono::time_point<std::chrono::high_resolution_clock> start_time;
  double cumlative_time;
  tile_hanlder tile_hndr;
  codestream cs;

  ReleaseSlabCb release_slab_cb_;
  void *release_slab_arg_;

  void release_held_slabs() {
    if (release_slab_cb_) {
      for (size_t idx : held_slabs_) release_slab_cb_(release_slab_arg_, idx);
    }
    held_slabs_.clear();
    chain_total_bytes_ = 0;
    cs.clear();
  }

 public:
  frame_handler()
      : chain_total_bytes_(0),
        total_frames(0),
        trunc_frames(0),
        lost_frames(0),
        start_SOD(0),
        is_parsing_failure(0),
        is_passed_header(0),
        cumlative_time(0.0),
        cs(),
        release_slab_cb_(nullptr),
        release_slab_arg_(nullptr) {
    start_time = std::chrono::high_resolution_clock::now();
    held_slabs_.reserve(2048);
  }

  ~frame_handler() {
    // Don't release slabs from destructor — receiver may already be gone.
    held_slabs_.clear();
  }

  size_t get_total_frames() const { return total_frames; }

  void set_precinct_callback(tile_hanlder::PrecinctReadyCb cb, void *arg) {
    tile_hndr.set_precinct_callback(cb, arg);
  }

  void set_parse_holdback(uint32_t n) { tile_hndr.set_parse_holdback(n); }
  uint32_t get_parse_holdback() const { return tile_hndr.get_parse_holdback(); }

  void set_release_slab_callback(ReleaseSlabCb cb, void *arg) {
    release_slab_cb_  = cb;
    release_slab_arg_ = arg;
  }

#ifdef PARSER_OVERSHOOT_INSTR
  tile_hanlder::OvershootStats get_overshoot_stats() const { return tile_hndr.get_overshoot_stats(); }
  void reset_overshoot_stats() { tile_hndr.reset_overshoot_stats(); }
#endif

  double get_cumlative_time_then_reset() {
    double ret     = cumlative_time;
    cumlative_time = 0.0;
    return ret;
  }

  double get_duration() {
    auto duration = std::chrono::high_resolution_clock::now() - start_time;
    auto count    = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    start_time    = std::chrono::high_resolution_clock::now();
    return static_cast<double>(count) / 1000;
  }

  inline void countup_lost_frames() { this->lost_frames++; }
  inline size_t get_lost_frames() const { return this->lost_frames; }
  inline size_t get_trunc_frames() const { return this->trunc_frames; }

  void pull_data(uint8_t *__restrict__ payload, size_t size, int marker, size_t slab_idx) {
    // Safety: if EOC has been missed for many packets, held_slabs_ would otherwise grow
    // unbounded and exhaust the receiver's slot ring (causing busy/net drop cascades).
    // 3072 leaves ~1024 slots of headroom in a 4096-slot ring — enough for a couple of
    // typical frames in flight while preventing runaway. After firing, we wait for the
    // next MH packet before resuming parsing (otherwise cs.reset(start_SOD) would
    // position us inside a body chunk).
    if (held_slabs_.size() >= 3072) {
      release_held_slabs();
      tile_hndr.restart(0);
      trunc_frames += 1;
      is_parsing_failure = 0;
      is_passed_header   = 0;
    }

    const uint32_t MH = payload[0] >> 6;

    // Skip body packets while not actively tracking a frame (right after cap, after a
    // parse failure, or before the first MH). Release the slab immediately so the
    // receiver can recycle it. We resume on the next MH packet.
    if (MH == 0 && (!is_passed_header || is_parsing_failure)) {
      if (release_slab_cb_) release_slab_cb_(release_slab_arg_, slab_idx);
      if (marker) {
        // EOC of a failed/abandoned frame. Count it as truncated if we were tracking
        // anything (is_parsing_failure indicates a parse error mid-frame; is_passed_header
        // would still be 1 for the rare case where the EOC body packet itself failed
        // before this branch). Either way, increment trunc + total to keep counts honest.
        if (is_parsing_failure || is_passed_header) {
          trunc_frames++;
          total_frames++;
        }
        is_parsing_failure = 0;
        is_passed_header   = 0;
        release_held_slabs();
        tile_hndr.restart(0);
      }
      return;
    }

    const uint32_t ORDB    = (payload[1] >> 7) & 1u;  // RFC 9828 body header: resync-present
    const uint32_t POS_PID = __builtin_bswap32(*(uint32_t *)(payload + 4));
    const uint32_t PID     = POS_PID & 0x000FFFFF;    // valid only when ORDB=1
    uint8_t *__restrict__ j2k_payload = payload + 8;

    if (MH >= 1) {  // Main packet — start of a new frame's main header bytes.
      log_init(total_frames);
      // Defensive: if we missed previous frame's EOC, the chain still holds stale
      // chunks. Release them so this MH starts a fresh chain.
      if (!held_slabs_.empty()) {
        release_held_slabs();
        tile_hndr.restart(0);
        if (is_passed_header) {
          trunc_frames++;
          total_frames++;
        }
        is_passed_header = 0;
      }
      is_parsing_failure = 0;
      cs.append_chunk(j2k_payload, size);
      held_slabs_.push_back(slab_idx);
      chain_total_bytes_ += size;

      if (!tile_hndr.is_ready()) {
        if (MH >= 2) {  // complete main header in this packet
          start_SOD = parse_main_header(&cs, tile_hndr.get_siz(), tile_hndr.get_cod(), tile_hndr.get_cocs(),
                                        tile_hndr.get_qcd(), tile_hndr.get_dfs());
          tile_hndr.create(&cs);
          is_passed_header = 1;
        } else {
          start_SOD += size;
        }
        if (start_SOD == 0) {
          return;  // something wrong
        }
      } else {
        // Subsequent frames: position the codestream reader at start_SOD within the
        // just-appended MH chunk. parse_main_header would have done this for the first
        // frame; for re-used main headers, we do it explicitly.
        cs.reset(start_SOD);
        is_passed_header = 1;
      }
    } else {
      // Body packet (and we're actively parsing): append its J2K bytes to the chain.
      cs.append_chunk(j2k_payload, size);
      held_slabs_.push_back(slab_idx);
      // Note resync byte before bumping the running total.
      const uint32_t POS         = POS_PID >> 20;
      const uint32_t resync_byte = static_cast<uint32_t>(chain_total_bytes_) + POS;
      chain_total_bytes_ += size;
      if (ORDB && is_passed_header) {
        tile_hndr.append_signal(resync_byte, PID);
        if (!is_parsing_failure) {
          ACTION(parse, PID);
        }
      }
    }

    if (marker) {  // EOC of this frame
      if (!is_parsing_failure && is_passed_header) {
        ACTION(flush);
      }
      trunc_frames += is_parsing_failure;
      total_frames++;

      log_close();
      is_parsing_failure = 0;
      is_passed_header   = 0;

      // Release held slabs back to the receiver and clear the chain.
      release_held_slabs();

      // Reset per-frame parser state (tag-trees, crp_idx, signal_queue). cs is already
      // cleared by release_held_slabs; tile_hndr.restart no longer touches cs.
      tile_hndr.restart(start_SOD);
    }
  }
};
}  // namespace j2k

#endif  // FRAME_HANDLER_HPP
