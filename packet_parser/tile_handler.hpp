//
// Created by OSAMU WATANABE on 2024/12/04.
//

#ifndef TILE_HANDLER_HPP
#define TILE_HANDLER_HPP

#include <cstdint>
#include <cassert>
#include <deque>
#include <vector>
#include "j2k_packet.hpp"
#include "utils.hpp"

class tile_hanlder {
 public:
  // Fired after each successful parse_one_precinct(), in PID order, on the parser thread.
  // Keep work minimal — long callbacks defeat the sub-codestream-latency goal.
  using PrecinctReadyCb = void (*)(void *user, const prec_ *pp, uint8_t c, uint8_t r, uint16_t p);

#ifdef PARSER_OVERSHOOT_INSTR
  // After each parse_one_precinct() that lands at a signaled byte boundary, we compare
  // parser_end against the authoritative resync byte. A mismatch means the parser
  // disagreed with the encoder about the precinct's end — drift caught and corrected by
  // the snap. failed_parses records parse errors with the precinct coordinates.
  struct OvershootStats {
    size_t precincts_parsed   = 0;
    size_t sum_precinct_bytes = 0;
    size_t snaps_with_drift   = 0;  // snap fired with non-zero correction
    size_t max_drift_bytes    = 0;
    size_t sum_drift_bytes    = 0;
    size_t failed_parses      = 0;
    uint32_t last_fail_c      = 0;
    uint32_t last_fail_r      = 0;
    uint32_t last_fail_p      = 0;
    uint32_t last_fail_crp_idx = 0;
    uint32_t last_fail_src_pos = 0;
  };
  OvershootStats get_overshoot_stats() const { return ostats_; }
  void reset_overshoot_stats() { ostats_ = OvershootStats{}; }
#endif

 private:
  std::vector<tile_> tiles;
  siz_marker siz;
  cod_marker cod;
  coc_marker cocs[MAX_NUM_COMPONENTS];
  qcd_marker qcd;
  dfs_marker dfs;
  uint32_t num_tiles_x;
  uint32_t num_tiles_y;
  bool ready;
  PrecinctReadyCb prec_cb_;
  void *prec_cb_arg_;
  // Number of precincts left unparsed behind the latest PID. Default 16 matches the
  // original hard-coded value; lowering it has been observed to truncate every frame on
  // a real 4K stream, so the safe minimum for correctness is not yet established.
  // Likely culprits to investigate before lowering: (a) pull_data() ordering bug at
  // frame_handler.hpp:112 (memcpy runs before MH>=1 resets incoming_data_len), (b) the
  // uint32_t* cast in the same memcpy truncates the destination address to 4-byte units,
  // (c) RFC 9828 leaves bytes [0, POS-1] of resync packets unspecified — if they aren't
  // codestream bytes, raw concatenation desynchronises the parser. The 16-precinct
  // lookahead may be masking overshoot from one of these.
  uint32_t parse_holdback_;  // deprecated: byte-queue gate replaces precinct-count cushion

  // FIFO of byte offsets in incoming_data where signaled (ORDB=1) precinct boundaries
  // land. Each ORDB=1 RTP packet appends one entry. Entries are guaranteed monotonic in
  // arrival order (= byte order). parse() uses signal_queue_.back() as the high-water
  // mark to gate parsing — only parse precincts whose body bytes are surely buffered.
  // After each parse_one_precinct, snap_to_signals() pops entries the parser passed and
  // corrects src on drift. PID is intentionally NOT used as an index here: per RFC 9828
  // PID = c + s*num_components, which is encoder-specific and does not map 1:1 to the
  // parser's crp_idx (PCRL y,x,c,r) order.
  std::deque<uint32_t> signal_queue_;

#ifdef PARSER_OVERSHOOT_INSTR
  OvershootStats ostats_;
#endif

 public:
  tile_hanlder()
      : tiles({}),
        siz({}),
        cod({}),
        cocs(),
        qcd({}),
        dfs({}),
        num_tiles_x(0),
        num_tiles_y(0),
        ready(false),
        prec_cb_(nullptr),
        prec_cb_arg_(nullptr),
        parse_holdback_(0) {}

  void set_precinct_callback(PrecinctReadyCb cb, void *arg) {
    prec_cb_     = cb;
    prec_cb_arg_ = arg;
  }

  // Optional EXTRA conservative cushion on top of the per-precinct end-of-body detection.
  // Default 0 (no cushion needed) since precinct_starts_ now provides authoritative ends.
  // Kept for A/B testing; setting non-zero just delays parsing by that many extra precincts.
  void set_parse_holdback(uint32_t n) { parse_holdback_ = n; }
  uint32_t get_parse_holdback() const { return parse_holdback_; }

  // Called by frame_handler each time a body packet with ORDB=1 arrives. byte_offset is
  // the absolute position in incoming_data of the resync point (start of some precinct's
  // packet header). Entries arrive in byte order (RTP packet seq order); the queue is a
  // straight FIFO. PID is intentionally not stored — see signal_queue_ comment.
  // The queue is normally drained per-frame via consume_passed_signals + clear in
  // restart(). The hard cap below guards against runaway growth if EOC packets are
  // persistently lost (16384 is ~2.5x one 4K@60 frame's precinct count, plenty of
  // headroom for legitimate bursts).
  void append_signal(uint32_t byte_offset) {
    if (signal_queue_.size() >= 16384) signal_queue_.clear();
    signal_queue_.push_back(byte_offset);
  }
  siz_marker *get_siz() { return &siz; }
  cod_marker *get_cod() { return &cod; }
  coc_marker *get_cocs() { return cocs; }
  qcd_marker *get_qcd() { return &qcd; }
  dfs_marker *get_dfs() { return &dfs; }
  bool is_ready() { return ready; }
  void create(codestream *buf) {
    num_tiles_x = ceildiv_int((siz.Xsiz - siz.XTOsiz), siz.XTsiz);
    num_tiles_y = ceildiv_int((siz.Ysiz - siz.YTOsiz), siz.YTsiz);

    if (num_tiles_x && num_tiles_y) tiles.reserve(num_tiles_x * num_tiles_y);

    for (uint32_t t = 0; t < num_tiles_x && num_tiles_y; ++t) {
      tiles.emplace_back(t, cocs[0].progression_order);
      tile_ *tile          = tiles.data() + t;
      uint32_t p           = t % num_tiles_x;
      uint32_t q           = t / num_tiles_y;
      tile->buf            = buf;
      tile->idx            = t;
      tile->num_components = siz.Csiz;
      tile->coord.x0       = LOCAL_MAX(siz.XTOsiz + p * siz.XTsiz, siz.XOsiz);
      tile->coord.y0       = LOCAL_MAX(siz.YTOsiz + q * siz.YTsiz, siz.YOsiz);
      tile->coord.x1       = LOCAL_MIN(siz.XTOsiz + (p + 1) * siz.XTsiz, siz.Xsiz);
      tile->coord.y1       = LOCAL_MIN(siz.YTOsiz + (q + 1) * siz.YTsiz, siz.Ysiz);
      {
        tile->progression_order = cocs[0].progression_order;
        for (uint32_t c = 0; c < tile->num_components; ++c) {
          tcomp_ *tcp     = &(tile->tcomp[c]);
          tcp->idx        = c;
          tcp->coord.x0   = ceildiv_int(tile->coord.x0, siz.XRsiz[c]);
          tcp->coord.y0   = ceildiv_int(tile->coord.y0, siz.YRsiz[c]);
          tcp->coord.x1   = ceildiv_int(tile->coord.x1, siz.XRsiz[c]);
          tcp->coord.y1   = ceildiv_int(tile->coord.y1, siz.YRsiz[c]);
          tcp->sub_x      = siz.XRsiz[c];
          tcp->sub_y      = siz.YRsiz[c];
          coc_marker *coc = &cocs[c];
          if (coc->NL > 32) {
            assert(dfs.idx == coc->NL - 128);
            coc->NL = dfs.Idfs;
          }
          parepare_tcomp_structure(tcp, coc, &dfs);
        }
        prepare_precinct_structure(tile, cocs, &dfs);
      }
    }
    ready = true;
  }

  int parse(uint32_t /*PID*/) {
    int ret     = EXIT_SUCCESS;
    tile_ *tile = tiles.data();

    // Each iteration:
    //   1. Pop signals at or before current src (signal_queue_.front() represents the
    //      start of the precinct we're about to parse — popping it is consuming, not
    //      drift). If src happened to be PAST the signal (parser drifted earlier),
    //      record drift but still pop.
    //   2. Gate: stop if no signals remain (can't bound parsing safely) or if src has
    //      already reached/passed the latest signal (the precinct starting there has a
    //      body of unknown extent and may not be fully buffered).
    //   3. Parse one precinct.
    while (true) {
      consume_passed_signals(tile);
      if (signal_queue_.empty()) break;
      if (static_cast<uint32_t>(tile->buf->get_pos()) >= signal_queue_.back()) break;

      const crp_status ct = tile->crp[tile->crp_idx];
#ifdef PARSER_OVERSHOOT_INSTR
      const size_t before_pos = tile->buf->get_pos();
      tile->buf->reset_max_offset_read();
#endif
      ret = parse_one_precinct(tile, this->cocs);
      tile->crp_idx++;
      if (ret) {
#ifdef PARSER_OVERSHOOT_INSTR
        record_failure(tile->buf, ct, tile->crp_idx - 1);
#endif
        break;
      }
#ifdef PARSER_OVERSHOOT_INSTR
      record_precinct(tile->buf, before_pos);
#endif
      if (prec_cb_) {
        const prec_ *pp = &tile->tcomp[ct.c].res[ct.r].prec[ct.p];
        prec_cb_(prec_cb_arg_, pp, ct.c, ct.r, ct.p);
      }
    }
    return ret;
  }

  int flush() {
    // EOC fires; all body bytes are buffered. Walk all remaining precincts. Pop signals
    // before each parse to track drift; no gate (we have everything).
    int ret     = EXIT_SUCCESS;
    tile_ *tile = tiles.data();
    const int n = static_cast<int>(tile->crp.size());
    for (; tile->crp_idx < n; tile->crp_idx++) {
      consume_passed_signals(tile);
      const crp_status ct = tile->crp[tile->crp_idx];
#ifdef PARSER_OVERSHOOT_INSTR
      const size_t before_pos = tile->buf->get_pos();
      tile->buf->reset_max_offset_read();
#endif
      ret = parse_one_precinct(tile, this->cocs);
      if (ret) {
#ifdef PARSER_OVERSHOOT_INSTR
        record_failure(tile->buf, ct, static_cast<uint32_t>(tile->crp_idx));
#endif
        break;
      }
#ifdef PARSER_OVERSHOOT_INSTR
      record_precinct(tile->buf, before_pos);
#endif
      if (prec_cb_) {
        const prec_ *pp = &tile->tcomp[ct.c].res[ct.r].prec[ct.p];
        prec_cb_(prec_cb_arg_, pp, ct.c, ct.r, ct.p);
      }
    }
    return ret;
  }

 private:
  // Pop any signals at or before current src. Called BEFORE each parse_one_precinct.
  // If src exactly equals a signal, that signal marks the precinct we're about to parse
  // — popping it is consuming, not drift. If src is past a signal, parser drifted past
  // a precinct without landing on its start byte; record drift but don't snap-back
  // (rewind would corrupt crp_idx alignment with src).
  void consume_passed_signals(tile_ *tile) {
    while (!signal_queue_.empty()) {
      const uint32_t parser_pos = static_cast<uint32_t>(tile->buf->get_pos());
      const uint32_t front      = signal_queue_.front();
      if (parser_pos < front) break;
#ifdef PARSER_OVERSHOOT_INSTR
      if (parser_pos > front) {
        const size_t drift = parser_pos - front;
        ostats_.snaps_with_drift++;
        ostats_.sum_drift_bytes += drift;
        if (drift > ostats_.max_drift_bytes) ostats_.max_drift_bytes = drift;
      }
#endif
      signal_queue_.pop_front();
    }
  }

#ifdef PARSER_OVERSHOOT_INSTR
  void record_precinct(codestream *buf, size_t before_pos) {
    const size_t after_pos = buf->get_pos();
    ostats_.precincts_parsed++;
    ostats_.sum_precinct_bytes += (after_pos - before_pos);
  }

  void record_failure(codestream *buf, const crp_status &ct, uint32_t crp_idx) {
    ostats_.failed_parses++;
    ostats_.last_fail_c        = ct.c;
    ostats_.last_fail_r        = ct.r;
    ostats_.last_fail_p        = ct.p;
    ostats_.last_fail_crp_idx  = crp_idx;
    ostats_.last_fail_src_pos  = buf->get_pos();
  }
#endif

 public:

  int read() {
    int ret;
    for (uint32_t t = 0; t < this->num_tiles_x * this->num_tiles_y; ++t) {
      ret = read_tile(tiles.data() + t, this->cocs, &this->dfs);
      if (ret) {
        return ret;
      }
    }
    return EXIT_SUCCESS;
  }

  void restart(uint32_t start_SOD) {
    // New frame: clear the signal queue. The frame's first body packet will append
    // start_SOD as its first signal (PID=0, POS=0 ⇒ byte_offset = size_MH = start_SOD).
    signal_queue_.clear();
    for (uint32_t t = 0; t < num_tiles_x * num_tiles_y; ++t) {
      tile_ *tile   = &tiles[t];
      tile->crp_idx = 0;
      tile->buf->reset(start_SOD);
      for (uint32_t c = tile->num_components; c > 0; --c) {
        tcomp_ *tcp = &(tile->tcomp[c - 1]);
        for (uint32_t r = MAX_DWT_LEVEL + 1; r > 0; --r) {
          const res_ *res              = &(tcp->res[r - 1]);
          const uint32_t num_precincts = res->npw * res->nph;
          for (uint32_t p = num_precincts; p > 0; --p) {
            const prec_ *prec = &res->prec[p - 1];
            for (uint32_t bp = prec->num_bands; bp > 0; --bp) {
              pband_ *pband = &prec->pband[bp - 1];
              tag_tree_zero(pband->incl, prec->ncbw, prec->ncbh, 0);
              tag_tree_zero(pband->zbp, prec->ncbw, prec->ncbh, 0);
              const uint32_t num_cblks = prec->ncbw * prec->ncbh;
              for (uint32_t n = num_cblks; n > 0; --n) {
                blk_ *blk   = &pband->blk[n - 1];
                blk->length = 0;
                // blk->incl            = 0;
                // blk->zbp             = 0;
                blk->npasses         = 0;
                blk->pass_lengths[0] = blk->pass_lengths[1] = 0;
              }
            }
          }
        }
      }
    }
    stackAlloc(0, 1);
  }
};

#endif  // TILE_HANDLER_HPP
