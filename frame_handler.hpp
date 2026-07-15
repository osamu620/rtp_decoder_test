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

  // Fired once per completed frame at EOC (see set_frame_ready_callback). Declared here,
  // ahead of the data members that reference it.
  using FrameReadyCb = void (*)(void *user, const codestream &cs, bool intact);

  // ---- Incremental (sub-frame) delivery — see set_chunk_callback ----
  // Fired for EVERY byte range appended to the current frame's codestream, in codestream
  // order, as it arrives: the main-header packet first (offset 0), then each body
  // packet's J2K bytes. Guarantees, per frame: offsets are strictly contiguous
  // (offset_n+1 == offset_n + len_n) and no chunk is delivered after the frame dies —
  // a frame ends in EITHER frame_ready (EOC; all bytes were delivered) OR frame_abort,
  // never both mid-stream. `bytes` is valid ONLY during the call (receiver slab memory).
  // An arrival-chasing consumer (e.g. a hardware decoder fed behind the RTP write
  // cursor) can copy/forward each range immediately.
  using ChunkCb = void (*)(void *user, size_t offset, const uint8_t *bytes, size_t len);

  // Fired at most ONCE per frame, the moment an in-flight frame becomes undecodable:
  //   kAbortGap        — packet loss immediately before this packet (pull_data gap=true)
  //   kAbortParse      — main-header or precinct parse failure
  //   kAbortMissedEOC  — a new frame's main packet arrived while this one was still open
  //   kAbortSlabCap    — the held-slab runaway cap released the frame
  //   kAbortDamagedEOC — EOC reached on a frame already being skipped (belt-and-braces;
  //                      normally the abort already fired at the damage point)
  // After an abort, no further chunks are delivered until the next frame's main packet.
  // A parse failure at/after EOC (flush) does NOT abort: every byte was already
  // delivered, so a byte-stream consumer's frame is complete regardless.
  using FrameAbortCb = void (*)(void *user, int reason);
  static constexpr int kAbortGap        = 1;
  static constexpr int kAbortParse      = 2;
  static constexpr int kAbortMissedEOC  = 3;
  static constexpr int kAbortSlabCap    = 4;
  static constexpr int kAbortDamagedEOC = 5;

  // ---- Stream re-latch (mid-stream encoder re-dial recovery, 2026-07-15) ----
  // The main header used to be parsed ONCE; every later frame reused the latched
  // start_SOD + tile structure, so an encoder re-dial (e.g. an i5x3 -> i9x7 kernel
  // flip) garbage-parsed every subsequent frame — a permanent parse-failure loop
  // until relaunch (field wedge, 2026-07-14). Now every complete main header's
  // geometry signature (j2k_header.hpp geometry_signature) is verified before the
  // latched state is reused; on mismatch the stream is RE-LATCHED: full header
  // re-parse + tile_handler re-create, then this callback fires (worker thread,
  // AFTER the new structure is live). Stream-scoped consumer caches (e.g. a PID
  // resync map) must be dropped in it. Reasons:
  //   kRelatchGeometry  — clean main header whose geometry signature differs from
  //                       the latched stream's (rate-only re-dials hash equal and
  //                       do NOT restart)
  //   kRelatchParseFail — escape hatch: set_relatch_parse_fail_k() consecutive
  //                       frames died in PARSE failures (gap/loss frames are
  //                       neutral), so the latched structure itself is suspect;
  //                       re-latched from the next clean main header
  using StreamRelatchCb = void (*)(void *user, int reason);
  static constexpr int kRelatchGeometry  = 1;
  static constexpr int kRelatchParseFail = 2;

  // ---- Transport-assisted mid-frame resync (C2 Stage C; consumer = a decoder
  // that can resume at an RFC 9828 resync point, e.g. the H2L1 PL).
  // ResyncGapCb: fired INSTEAD of the kAbortGap abort when a gap hits a frame in
  // flight. `seam` = bytes delivered so far (the chain is COMPACTED: post-gap
  // chunks keep appending right after it). Return true to ARM the resync — the
  // frame keeps delivering; false = decline, today's kAbortGap abort fires.
  // ResyncPointCb: fired for EACH ORDB resync point after an armed gap, with the
  // point's arrival-space byte offset (chain_total at the packet + POS) and its
  // PID. Return true once the consumer has accepted a point (disarms the offer
  // stream); false = keep offering later points. The software precinct walk
  // stays live and self-recovers via its own signal queue (try_recover).
  using ResyncGapCb   = bool (*)(void *user, size_t seam);
  using ResyncPointCb = bool (*)(void *user, size_t resync_byte, uint32_t pid);

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
  tile_handler tile_hndr;
  codestream cs;

  ReleaseSlabCb release_slab_cb_;
  void *release_slab_arg_;

  FrameReadyCb frame_ready_cb_ = nullptr;
  void *frame_ready_arg_       = nullptr;

  ChunkCb chunk_cb_            = nullptr;
  void *chunk_arg_             = nullptr;
  FrameAbortCb frame_abort_cb_ = nullptr;
  void *frame_abort_arg_       = nullptr;
  ResyncGapCb resync_gap_cb_     = nullptr;
  void *resync_gap_arg_          = nullptr;
  ResyncPointCb resync_point_cb_ = nullptr;
  void *resync_point_arg_        = nullptr;
  StreamRelatchCb relatch_cb_ = nullptr;
  void *relatch_arg_          = nullptr;
  uint64_t geom_sig_          = 0;  // signature of the latched stream (0 = none/unknown)
  uint32_t relatch_k_         = 4;  // parse-fail hatch threshold in frames (0 = hatch off)
  uint32_t parse_fail_streak_ = 0;  // consecutive frames dead by PARSE failure
  bool frame_parse_failed_    = false;  // current frame died by a parse failure (not a gap)
  size_t relatches_           = 0;
  bool resync_armed_ = false;  // gap accepted by the consumer; offering points
  // True from the first accepted gap until the frame ends: the SOFTWARE precinct
  // walk is parked (it would garbage-parse at the compaction seam and abort the
  // frame — the resync consumer's own decoder handles the seam), while chunk
  // delivery continues. The walk restarts clean for the next frame.
  bool resync_soft_ = false;
  // True while a frame is open and clean from the byte-delivery perspective: set when
  // its first chunk is delivered, cleared at EOC handoff or by fire_abort. Guarantees
  // the at-most-one-abort-per-frame contract.
  bool abort_armed_ = false;

  void deliver_chunk(size_t offset, const uint8_t *bytes, size_t len) {
    abort_armed_ = true;
    if (chunk_cb_) chunk_cb_(chunk_arg_, offset, bytes, len);
  }

  void fire_abort(int reason) {
    if (reason == kAbortParse) frame_parse_failed_ = true;  // hatch evidence (gaps are neutral)
    resync_armed_ = false;  // the frame is over from the resync offer's perspective
    resync_soft_  = false;
    if (!abort_armed_) return;
    abort_armed_ = false;
    if (frame_abort_cb_) frame_abort_cb_(frame_abort_arg_, reason);
  }

  // Frame-end accounting for the parse-fail escape hatch: parse-failed frames grow the
  // streak, clean frames reset it, gap/loss-failed frames are NEUTRAL (loss says nothing
  // about whether the latched structure still matches the stream — under heavy loss the
  // hatch must not churn re-latches, and under a silent re-dial loss must not mask it).
  void end_frame_streak(bool clean) {
    if (clean)
      parse_fail_streak_ = 0;
    else if (frame_parse_failed_)
      parse_fail_streak_++;
    frame_parse_failed_ = false;
  }

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

  void set_precinct_callback(tile_handler::PrecinctReadyCb cb, void *arg) {
    tile_hndr.set_precinct_callback(cb, arg);
  }

  void set_parse_holdback(uint32_t n) { tile_hndr.set_parse_holdback(n); }
  uint32_t get_parse_holdback() const { return tile_hndr.get_parse_holdback(); }

  void set_release_slab_callback(ReleaseSlabCb cb, void *arg) {
    release_slab_cb_  = cb;
    release_slab_arg_ = arg;
  }

  // Frame-ready callback: fired once per completed frame at EOC, BEFORE the held slabs
  // are released — so `cs`'s zero-copy chain is still valid for the duration of the call.
  // `intact` is true iff the main header parsed and no precinct parse failure occurred
  // (false = damaged: lost packets or a parse failure). A frame-granular consumer (e.g.
  // an FPGA whole-frame decoder) can pull the contiguous codestream via cs.for_each_chunk()
  // inside the callback. Independent of (and composable with) the per-precinct callback.
  void set_frame_ready_callback(FrameReadyCb cb, void *arg) {
    frame_ready_cb_  = cb;
    frame_ready_arg_ = arg;
  }

  // Incremental delivery (see the ChunkCb/FrameAbortCb contracts above). Both are
  // independent of — and composable with — the per-precinct and frame-ready callbacks;
  // unset (the default) they cost one nullptr test per packet and change nothing.
  // For loss-exactness the caller should pass `gap` into pull_data (see there).
  void set_chunk_callback(ChunkCb cb, void *arg) {
    chunk_cb_  = cb;
    chunk_arg_ = arg;
  }

  void set_frame_abort_callback(FrameAbortCb cb, void *arg) {
    frame_abort_cb_  = cb;
    frame_abort_arg_ = arg;
  }

  // C2 Stage C (see the typedefs above): register a resync-capable consumer.
  // Both must be set for the gap-accept path to engage.
  void set_resync_callbacks(ResyncGapCb gap_cb, void *gap_arg,
                            ResyncPointCb point_cb, void *point_arg) {
    resync_gap_cb_    = gap_cb;
    resync_gap_arg_   = gap_arg;
    resync_point_cb_  = point_cb;
    resync_point_arg_ = point_arg;
  }

  // Stream re-latch notification + tuning (see the typedef/constants above). The
  // re-latch itself is always on — the callback is only observation/cache-drop.
  void set_stream_relatch_callback(StreamRelatchCb cb, void *arg) {
    relatch_cb_  = cb;
    relatch_arg_ = arg;
  }
  // K consecutive parse-failed frames trigger the escape-hatch re-latch (default 4
  // ≈ 67 ms at 60 fps; 0 disables the hatch — the geometry path stays active).
  void set_relatch_parse_fail_k(uint32_t k) { relatch_k_ = k; }
  size_t get_relatches() const { return relatches_; }

#ifdef PARSER_OVERSHOOT_INSTR
  tile_handler::OvershootStats get_overshoot_stats() const { return tile_hndr.get_overshoot_stats(); }
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

  // `gap` (optional): true iff one or more RTP packets were lost immediately before this
  // one (an RTP sequence discontinuity, computed by the caller's hook — the receiver
  // delivers in order, so a seq skip IS a loss). With a frame in flight this makes loss
  // detection immediate and deterministic for the incremental consumer: the frame is
  // aborted at the gap instead of waiting for a downstream parse failure to notice.
  // Callers that don't track seq (the default) keep today's parse-level detection.
  void pull_data(uint8_t *__restrict__ payload, size_t size, int marker, size_t slab_idx,
                 bool gap = false) {
    // A gap kills any in-flight frame: bytes are missing, so neither the parser nor a
    // byte-stream consumer can use what follows. Mark it failed — the body-skip path
    // below then drops packets until the next main packet resyncs us. (A gap with
    // nothing in flight lost only packets of frames we never started: nothing to do.)
    // C2 Stage C exception: a resync-capable consumer may ACCEPT the gap instead —
    // the chain then keeps delivering (compacted) and ORDB points are offered until
    // the consumer takes one. Declined/absent consumer = today's abort, unchanged.
    if (gap && (is_passed_header || !held_slabs_.empty()) && !is_parsing_failure) {
      if (resync_gap_cb_ && is_passed_header &&
          resync_gap_cb_(resync_gap_arg_, chain_total_bytes_)) {
        resync_armed_ = true;
        resync_soft_  = true;  // park the software walk for the rest of the frame
      } else {
        fire_abort(kAbortGap);
        is_parsing_failure = 1;
      }
    }

    // Safety: if EOC has been missed for many packets, held_slabs_ would otherwise grow
    // unbounded and exhaust the receiver's slot ring (causing busy/net drop cascades).
    // 3072 leaves ~1024 slots of headroom in a 4096-slot ring — enough for a couple of
    // typical frames in flight while preventing runaway. After firing, we wait for the
    // next MH packet before resuming parsing (otherwise cs.reset(start_SOD) would
    // position us inside a body chunk).
    if (held_slabs_.size() >= 3072) {
      fire_abort(kAbortSlabCap);
      release_held_slabs();
      tile_hndr.restart(0);
      trunc_frames += 1;
      end_frame_streak(false);
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
          fire_abort(kAbortDamagedEOC);  // normally a no-op: the abort fired at the damage
          trunc_frames++;
          total_frames++;
          end_frame_streak(false);
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
        fire_abort(kAbortMissedEOC);  // the open frame will never complete
        release_held_slabs();
        tile_hndr.restart(0);
        // Count the abandoned frame once. is_parsing_failure covers a frame rejected at
        // create()/parse whose EOC was lost on the wire (is_passed_header never set);
        // mirrors the EOC early-skip block's condition so counts stay consistent.
        if (is_passed_header || is_parsing_failure) {
          trunc_frames++;
          total_frames++;
          end_frame_streak(false);
        }
        is_passed_header = 0;
      }
      is_parsing_failure  = 0;
      frame_parse_failed_ = false;
      deliver_chunk(chain_total_bytes_, j2k_payload, size);
      cs.append_chunk(j2k_payload, size);
      held_slabs_.push_back(slab_idx);
      chain_total_bytes_ += size;

      // Stream re-latch guard (complete-main-header packets): verify the geometry
      // signature BEFORE trusting the latched start_SOD/tile structure. A mid-stream
      // encoder re-dial (e.g. an i5x3 -> i9x7 kernel flip) used to garbage-parse every
      // subsequent frame against the stale structure — a permanent parse-failure loop
      // until relaunch (field wedge, 2026-07-14).
      uint64_t sig = 0;
      uint32_t sod = 0;
      int relatch  = 0;
      if (MH >= 2) {
        sig = geometry_signature(j2k_payload, size, &sod);
        if (sig == 0 && tile_hndr.is_ready()) {
          // Malformed main header on a latched stream: refuse to reuse latched state
          // on its say-so (the old path blindly cs.reset(start_SOD) and garbage-walked).
          fire_abort(kAbortParse);
          is_parsing_failure = 1;
          return;
        }
        if (tile_hndr.is_ready()) {
          if (geom_sig_ == 0) geom_sig_ = sig;  // adopt: stream was latched via MH==1 packets
          if (relatch_k_ && parse_fail_streak_ >= relatch_k_)
            relatch = kRelatchParseFail;  // escape hatch: the latched structure is suspect
          else if (sig != geom_sig_)
            relatch = kRelatchGeometry;  // clean header, new geometry
          if (relatch) tile_hndr.invalidate();  // full re-parse + create() below
        }
      }

      if (!tile_hndr.is_ready()) {
        if (MH >= 2) {  // complete main header in this packet
          cs.reset(0);  // defensive: the chain holds exactly this MH chunk (see clear())
          start_SOD = parse_main_header(&cs, tile_hndr.get_siz(), tile_hndr.get_cod(), tile_hndr.get_cocs(),
                                        tile_hndr.get_qcd(), tile_hndr.get_dfs());
          if (start_SOD == 0 || !tile_hndr.create(&cs)) {
            // Malformed/desynced main header, or an unsupported progression order:
            // fail the frame cleanly. Body packets are then dropped and the frame is
            // counted as truncated at EOC, instead of proceeding with an empty/invalid
            // precinct structure (or risking a non-terminating main-header scan).
            // (On a failed RE-latch tile_hndr stays not-ready, so every following MH
            // retries the full parse until a good header arrives — the resync scan.)
            fire_abort(kAbortParse);
            is_parsing_failure = 1;
            return;
          }
          is_passed_header   = 1;
          geom_sig_          = sig;
          parse_fail_streak_ = 0;
          if (relatch) {  // fired AFTER the new structure is live (worker thread)
            relatches_++;
            if (relatch_cb_) relatch_cb_(relatch_arg_, relatch);
          }
        } else {
          start_SOD += static_cast<uint32_t>(size);
          if (start_SOD == 0) {
            return;  // something wrong
          }
        }
      } else {
        // Subsequent frames, signature verified equal (or an MH==1 split header:
        // unverifiable — the parse-fail escape hatch above still covers a silent
        // re-dial). sod is authoritative from the fresh walk, so benign main-header
        // length drift (a COM change etc.) self-heals instead of desyncing the walk.
        if (sod) start_SOD = sod;
        cs.reset(start_SOD);
        is_passed_header = 1;
      }
    } else {
      // Body packet (and we're actively parsing): append its J2K bytes to the chain.
      deliver_chunk(chain_total_bytes_, j2k_payload, size);
      cs.append_chunk(j2k_payload, size);
      held_slabs_.push_back(slab_idx);
      // Note resync byte before bumping the running total.
      const uint32_t POS         = POS_PID >> 20;
      const uint32_t resync_byte = static_cast<uint32_t>(chain_total_bytes_) + POS;
      chain_total_bytes_ += size;
      if (ORDB && is_passed_header) {
        tile_hndr.append_signal(resync_byte, PID);
        // C2 Stage C: offer each post-gap resync point until the consumer takes one
        if (resync_armed_ && resync_point_cb_ &&
            resync_point_cb_(resync_point_arg_, resync_byte, PID))
          resync_armed_ = false;
        if (!is_parsing_failure && !resync_soft_) {
          ACTION(parse, PID);
          if (is_parsing_failure) fire_abort(kAbortParse);  // ACTION just set it
        }
      }
    }

    if (marker) {  // EOC of this frame
      // Byte delivery is complete: disarm the abort. A flush() parse failure below no
      // longer aborts — every byte reached the incremental consumer, whose own decoder
      // judges the stream for itself.
      abort_armed_  = false;
      resync_armed_ = false;  // C2 Stage C: no points beyond the frame
      // A resynced frame is complete for the BYTE consumer but its software walk
      // is parked (and the chain is compacted) — flushing would garbage-parse.
      const bool frame_intact = !is_parsing_failure && is_passed_header && !resync_soft_;
      resync_soft_ = false;
      if (frame_intact) {
        ACTION(flush);
        // A flush failure doesn't abort (all bytes were delivered) but IS structure-vs-
        // stream evidence for the parse-fail escape hatch, same as a mid-frame failure.
        if (is_parsing_failure) frame_parse_failed_ = true;
      }
      end_frame_streak(frame_intact && !is_parsing_failure);
      // Write the frame's codestream before the slabs are released — the chain points
      // into the receiver's slab ring. Uses the same frame number as log_init so
      // out_NNNNN.j2c pairs with log_NNNNN.log.
      save_j2c(total_frames, cs);
      // Hand the completed frame to a frame-granular consumer while cs's chain is still
      // valid (the held slabs are released just below). intact=false => damaged frame.
      if (frame_ready_cb_) frame_ready_cb_(frame_ready_arg_, cs, frame_intact);
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
