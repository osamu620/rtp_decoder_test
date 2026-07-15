// incremental_delivery_test — contract test for frame_handler's incremental
// (sub-frame) delivery: the chunk callback, the frame-abort callback, and the
// pull_data `gap` parameter.
//
// Feeds a real codestream through frame_handler as RFC 9828-shaped packets
// ([8B sub-header][J2K bytes], the post-RTP-header form pull_data takes) and
// asserts, per scenario:
//   clean      — chunks are contiguous from offset 0 and their concatenation is
//                byte-exact the input; frame_ready fires; no abort.
//   gap        — dropping a mid-body packet (gap=true on the next) fires exactly
//                one kAbortGap, stops chunk delivery for that frame, suppresses
//                frame_ready, and the NEXT frame delivers clean.
//   missed EOC — a frame cut before its final packet aborts with kAbortMissedEOC
//                when the next frame's main packet arrives; the next frame is clean.
//   EOC gap    — losing the packet(s) right before the EOC packet aborts the frame
//                (kAbortGap) and never fires frame_ready for it.
//
// With a SECOND stream of DIFFERENT geometry (different image size — the hatch scenario
// needs the structures to actually disagree), the stream re-latch scenarios run too:
//   re-latch     — A,B,B,A: each geometry flip re-latches (kRelatchGeometry), zero
//                  aborts, every frame delivered clean and intact.
//   mid-flip     — A cut before EOC, then B (the 2026-07-14 field shape): one
//                  kAbortMissedEOC for the torn frame, B re-latches and is intact.
//   parse hatch  — hybrid frames (A's main header + B's body) parse-fail at flush K
//                  times, then the escape hatch re-latches (kRelatchParseFail) and a
//                  clean B recovers via a geometry re-latch.
// With a THIRD stream of the SAME geometry as A at a different rate:
//   rate-only    — A,C,A: signatures equal, relatches == 0 (the "except bit-rate"
//                  exemption), all frames clean.
//
// usage: incremental_delivery_test <streamA.j2c> [streamB_diffgeom.j2c] [streamC_samegeom.j2c]
//        (exit 0 pass / 1 fail / 2 bad input)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <frame_handler.hpp>

namespace {

std::vector<uint8_t> read_file(const char *path) {
  std::vector<uint8_t> v;
  FILE *f = std::fopen(path, "rb");
  if (!f) return v;
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n > 0) {
    v.resize((size_t)n);
    if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
  }
  std::fclose(f);
  return v;
}

// One packet as pull_data consumes it: [8B RFC 9828 sub-header][J2K bytes].
struct Pkt {
  std::vector<uint8_t> payload;
  int marker;
};

// Split a codestream into a main packet ([0, SOD-end)) + fixed-size body packets;
// the last body packet carries the EOC and gets the RTP marker. Mirrors the shape
// h2l1_rtp_packetizer.py / kdu_stream_send produce (ORDB=0: delivery contract only,
// no mid-frame parse — precinct parsing has its own test).
std::vector<Pkt> packetize(const std::vector<uint8_t> &cs, size_t body_chunk) {
  std::vector<Pkt> pkts;
  size_t sod = 0;
  for (size_t i = 0; i + 1 < cs.size(); i++)
    if (cs[i] == 0xFF && cs[i + 1] == 0x93) { sod = i + 2; break; }
  if (!sod) return pkts;

  auto push = [&](const uint8_t *p, size_t n, int mh, int marker) {
    Pkt k;
    k.payload.assign(8, 0);
    k.payload[0] = (uint8_t)(mh << 6);
    k.payload.insert(k.payload.end(), p, p + n);
    k.marker = marker;
    pkts.push_back(std::move(k));
  };
  push(cs.data(), sod, 3, 0);  // main packet: complete main header (MH=3)
  for (size_t off = sod; off < cs.size(); off += body_chunk) {
    size_t n = cs.size() - off < body_chunk ? cs.size() - off : body_chunk;
    push(cs.data() + off, n, 0, off + n == cs.size());
  }
  return pkts;
}

// Test consumer state shared by the callbacks.
struct Ctx {
  std::vector<uint8_t> got;     // concatenated chunk bytes for the CURRENT frame
  size_t next_off   = 0;        // contiguity cursor
  size_t contig_bad = 0;
  size_t frames_ready = 0, frames_intact = 0;
  std::vector<int> aborts;      // reasons, in order
  std::vector<int> relatches;   // stream re-latch reasons, in order
  bool dead = false;            // abort seen for the current frame
  size_t chunks_after_abort = 0;

  void reset_frame() {
    got.clear();
    next_off = 0;
    dead     = false;
  }
};

void on_chunk(void *u, size_t off, const uint8_t *b, size_t n) {
  auto *c = static_cast<Ctx *>(u);
  if (off == 0) c->reset_frame();  // new frame begins
  if (c->dead) { c->chunks_after_abort++; return; }
  if (off != c->next_off) c->contig_bad++;
  c->got.insert(c->got.end(), b, b + n);
  c->next_off = off + n;
}

void on_abort(void *u, int reason) {
  auto *c = static_cast<Ctx *>(u);
  c->aborts.push_back(reason);
  c->dead = true;
}

void on_ready(void *u, const codestream &, bool intact) {
  auto *c = static_cast<Ctx *>(u);
  c->frames_ready++;
  if (intact) c->frames_intact++;
}

void on_relatch(void *u, int reason) {
  auto *c = static_cast<Ctx *>(u);
  c->relatches.push_back(reason);
}

// Slab arena: pull_data holds slabs until frame_handler releases them.
struct Slabs {
  std::vector<std::vector<uint8_t>> mem;
  std::vector<int> held;
  size_t leaked() const {
    size_t n = 0;
    for (int h : held) n += (h != 0);
    return n;
  }
};

void release_slab(void *u, size_t idx) {
  auto *s = static_cast<Slabs *>(u);
  if (idx < s->held.size()) s->held[idx] = 0;
}

int fails = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      std::fprintf(stderr, "FAIL: " __VA_ARGS__);         \
      std::fprintf(stderr, "  [%s]\n", #cond);            \
      fails++;                                            \
    }                                                     \
  } while (0)

// Feed a packet list; skip[i]==true drops packet i (gap=true passed on the next
// delivered packet, like the receiver's force-advance does).
void feed(j2k::frame_handler &fh, Slabs &slabs, const std::vector<Pkt> &pkts,
          const std::vector<bool> *skip = nullptr) {
  bool pending_gap = false;
  for (size_t i = 0; i < pkts.size(); i++) {
    if (skip && (*skip)[i]) {
      pending_gap = true;
      continue;
    }
    size_t idx = slabs.mem.size();
    slabs.mem.push_back(pkts[i].payload);  // slab-lifetime copy
    slabs.held.push_back(1);
    fh.pull_data(slabs.mem[idx].data(), pkts[i].payload.size() - 8, pkts[i].marker, idx,
                 pending_gap);
    pending_gap = false;
  }
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <stream.j2c>\n", argv[0]);
    return 2;
  }
  std::vector<uint8_t> cs = read_file(argv[1]);
  if (cs.size() < 4 || cs[0] != 0xFF || cs[1] != 0x4F) {
    std::fprintf(stderr, "not a raw codestream: %s\n", argv[1]);
    return 2;
  }
  const std::vector<Pkt> pkts = packetize(cs, 1400);
  if (pkts.size() < 4) {
    std::fprintf(stderr, "stream too small to packetize\n");
    return 2;
  }
  std::printf("stream %s: %zu B -> %zu packets\n", argv[1], cs.size(), pkts.size());

  // ---- scenario 1: two clean frames ----
  {
    j2k::frame_handler fh;
    Ctx ctx;
    Slabs slabs;
    fh.set_release_slab_callback(&release_slab, &slabs);
    fh.set_chunk_callback(&on_chunk, &ctx);
    fh.set_frame_abort_callback(&on_abort, &ctx);
    fh.set_frame_ready_callback(&on_ready, &ctx);
    feed(fh, slabs, pkts);
    CHECK(ctx.got == cs, "clean f1: reassembled bytes != input (%zu vs %zu)\n", ctx.got.size(),
          cs.size());
    feed(fh, slabs, pkts);
    CHECK(ctx.got == cs, "clean f2: reassembled bytes != input\n");
    CHECK(ctx.contig_bad == 0, "clean: %zu non-contiguous chunks\n", ctx.contig_bad);
    CHECK(ctx.aborts.empty(), "clean: %zu aborts\n", ctx.aborts.size());
    CHECK(ctx.frames_ready == 2, "clean: frame_ready %zu != 2\n", ctx.frames_ready);
    CHECK(slabs.leaked() == 0, "clean: %zu slabs leaked\n", slabs.leaked());
  }

  // ---- scenario 2: mid-body gap aborts frame 1; frame 2 clean ----
  {
    j2k::frame_handler fh;
    Ctx ctx;
    Slabs slabs;
    fh.set_release_slab_callback(&release_slab, &slabs);
    fh.set_chunk_callback(&on_chunk, &ctx);
    fh.set_frame_abort_callback(&on_abort, &ctx);
    fh.set_frame_ready_callback(&on_ready, &ctx);
    std::vector<bool> skip(pkts.size(), false);
    skip[pkts.size() / 2] = true;  // drop one mid-body packet
    feed(fh, slabs, pkts, &skip);
    CHECK(ctx.aborts.size() == 1 && ctx.aborts[0] == j2k::frame_handler::kAbortGap,
          "gap: expected exactly one kAbortGap, got %zu aborts\n", ctx.aborts.size());
    CHECK(ctx.frames_ready == 0, "gap: frame_ready fired for a gapped frame\n");
    CHECK(ctx.chunks_after_abort == 0, "gap: %zu chunks delivered after abort\n",
          ctx.chunks_after_abort);
    feed(fh, slabs, pkts);
    CHECK(ctx.got == cs, "gap: frame 2 not delivered clean\n");
    CHECK(ctx.frames_ready == 1 && ctx.frames_intact == 1, "gap: frame 2 not intact\n");
    CHECK(ctx.aborts.size() == 1, "gap: extra aborts on the clean frame\n");
    CHECK(slabs.leaked() == 0, "gap: %zu slabs leaked\n", slabs.leaked());
  }

  // ---- scenario 3: missed EOC (frame 1 cut short, no marker) ----
  {
    j2k::frame_handler fh;
    Ctx ctx;
    Slabs slabs;
    fh.set_release_slab_callback(&release_slab, &slabs);
    fh.set_chunk_callback(&on_chunk, &ctx);
    fh.set_frame_abort_callback(&on_abort, &ctx);
    fh.set_frame_ready_callback(&on_ready, &ctx);
    std::vector<Pkt> cut(pkts.begin(), pkts.end() - 3);  // lose the tail incl. EOC
    feed(fh, slabs, cut);
    CHECK(ctx.aborts.empty(), "missed-eoc: abort before the next MH?\n");
    feed(fh, slabs, pkts);  // next frame's MH exposes the missed EOC
    CHECK(ctx.aborts.size() == 1 && ctx.aborts[0] == j2k::frame_handler::kAbortMissedEOC,
          "missed-eoc: expected one kAbortMissedEOC, got %zu aborts\n", ctx.aborts.size());
    CHECK(ctx.got == cs, "missed-eoc: frame 2 not delivered clean\n");
    CHECK(ctx.frames_ready == 1, "missed-eoc: frame_ready %zu != 1\n", ctx.frames_ready);
    CHECK(slabs.leaked() == 0, "missed-eoc: %zu slabs leaked\n", slabs.leaked());
  }

  // ---- scenario 4: loss right before the EOC packet ----
  {
    j2k::frame_handler fh;
    Ctx ctx;
    Slabs slabs;
    fh.set_release_slab_callback(&release_slab, &slabs);
    fh.set_chunk_callback(&on_chunk, &ctx);
    fh.set_frame_abort_callback(&on_abort, &ctx);
    fh.set_frame_ready_callback(&on_ready, &ctx);
    std::vector<bool> skip(pkts.size(), false);
    skip[pkts.size() - 2] = true;  // gap lands ON the EOC packet
    feed(fh, slabs, pkts, &skip);
    CHECK(ctx.aborts.size() == 1 && ctx.aborts[0] == j2k::frame_handler::kAbortGap,
          "eoc-gap: expected one kAbortGap, got %zu aborts\n", ctx.aborts.size());
    CHECK(ctx.frames_ready == 0, "eoc-gap: frame_ready fired on a torn frame\n");
    feed(fh, slabs, pkts);
    CHECK(ctx.got == cs && ctx.frames_ready == 1, "eoc-gap: frame 2 not clean\n");
    CHECK(slabs.leaked() == 0, "eoc-gap: %zu slabs leaked\n", slabs.leaked());
  }

  int scenarios = 4;

  // ---- stream re-latch scenarios (need a second, different-geometry stream) ----
  std::vector<uint8_t> csB;
  std::vector<Pkt> pktsB;
  if (argc >= 3) {
    csB = read_file(argv[2]);
    if (csB.size() < 4 || csB[0] != 0xFF || csB[1] != 0x4F) {
      std::fprintf(stderr, "not a raw codestream: %s\n", argv[2]);
      return 2;
    }
    pktsB = packetize(csB, 1400);
    if (pktsB.size() < 4) {
      std::fprintf(stderr, "stream B too small to packetize\n");
      return 2;
    }
    std::printf("stream %s: %zu B -> %zu packets\n", argv[2], csB.size(), pktsB.size());
  }

  // ---- scenario 5: geometry re-latch — A, B, B, A, all clean ----
  if (!pktsB.empty()) {
    j2k::frame_handler fh;
    Ctx ctx;
    Slabs slabs;
    fh.set_release_slab_callback(&release_slab, &slabs);
    fh.set_chunk_callback(&on_chunk, &ctx);
    fh.set_frame_abort_callback(&on_abort, &ctx);
    fh.set_frame_ready_callback(&on_ready, &ctx);
    fh.set_stream_relatch_callback(&on_relatch, &ctx);
    feed(fh, slabs, pkts);
    CHECK(ctx.got == cs, "relatch: frame A1 not delivered clean\n");
    feed(fh, slabs, pktsB);
    CHECK(ctx.got == csB, "relatch: frame B1 (the flip frame) not delivered clean\n");
    feed(fh, slabs, pktsB);
    CHECK(ctx.got == csB, "relatch: frame B2 (post-flip steady state) not delivered clean\n");
    feed(fh, slabs, pkts);
    CHECK(ctx.got == cs, "relatch: frame A2 (flip back) not delivered clean\n");
    CHECK(ctx.aborts.empty(), "relatch: %zu aborts on clean flips\n", ctx.aborts.size());
    CHECK(ctx.frames_ready == 4 && ctx.frames_intact == 4,
          "relatch: ready=%zu intact=%zu != 4/4 (the old one-time latch made every "
          "post-flip frame a parse failure)\n",
          ctx.frames_ready, ctx.frames_intact);
    CHECK(ctx.relatches.size() == 2 && ctx.relatches[0] == j2k::frame_handler::kRelatchGeometry
              && ctx.relatches[1] == j2k::frame_handler::kRelatchGeometry,
          "relatch: expected 2x kRelatchGeometry, got %zu relatches\n", ctx.relatches.size());
    CHECK(slabs.leaked() == 0, "relatch: %zu slabs leaked\n", slabs.leaked());
    scenarios++;
  }

  // ---- scenario 6: mid-stream flip on a torn frame (the 2026-07-14 field shape) ----
  if (!pktsB.empty()) {
    j2k::frame_handler fh;
    Ctx ctx;
    Slabs slabs;
    fh.set_release_slab_callback(&release_slab, &slabs);
    fh.set_chunk_callback(&on_chunk, &ctx);
    fh.set_frame_abort_callback(&on_abort, &ctx);
    fh.set_frame_ready_callback(&on_ready, &ctx);
    fh.set_stream_relatch_callback(&on_relatch, &ctx);
    feed(fh, slabs, pkts);                                   // latch stream A
    std::vector<Pkt> cut(pkts.begin(), pkts.end() - 3);      // A frame torn mid-arrival
    feed(fh, slabs, cut);
    feed(fh, slabs, pktsB);                                  // the re-dial lands here
    CHECK(ctx.aborts.size() == 1 && ctx.aborts[0] == j2k::frame_handler::kAbortMissedEOC,
          "mid-flip: expected one kAbortMissedEOC for the torn frame, got %zu aborts\n",
          ctx.aborts.size());
    CHECK(ctx.got == csB, "mid-flip: the first new-stream frame not delivered clean\n");
    CHECK(ctx.frames_ready == 2 && ctx.frames_intact == 2, "mid-flip: ready=%zu intact=%zu != 2/2\n",
          ctx.frames_ready, ctx.frames_intact);
    CHECK(ctx.relatches.size() == 1 && ctx.relatches[0] == j2k::frame_handler::kRelatchGeometry,
          "mid-flip: expected 1 geometry re-latch, got %zu\n", ctx.relatches.size());
    CHECK(slabs.leaked() == 0, "mid-flip: %zu slabs leaked\n", slabs.leaked());
    scenarios++;
  }

  // ---- scenario 7: parse-fail escape hatch — hybrid frames (A header + B body) ----
  if (!pktsB.empty()) {
    j2k::frame_handler fh;
    Ctx ctx;
    Slabs slabs;
    fh.set_release_slab_callback(&release_slab, &slabs);
    fh.set_chunk_callback(&on_chunk, &ctx);
    fh.set_frame_abort_callback(&on_abort, &ctx);
    fh.set_frame_ready_callback(&on_ready, &ctx);
    fh.set_stream_relatch_callback(&on_relatch, &ctx);
    fh.set_relatch_parse_fail_k(3);
    std::vector<Pkt> hybrid;                    // A's main header, B's body: the signature
    hybrid.push_back(pkts[0]);                  // matches the latched stream, but the body
    hybrid.insert(hybrid.end(), pktsB.begin() + 1, pktsB.end());  // parse-fails every frame
    feed(fh, slabs, pkts);                      // latch stream A
    for (int i = 0; i < 4; i++) feed(fh, slabs, hybrid);  // 3 grow the streak; #4 trips the hatch
    bool hatch_fired = false;
    for (int r : ctx.relatches) hatch_fired |= (r == j2k::frame_handler::kRelatchParseFail);
    CHECK(hatch_fired, "hatch: kRelatchParseFail never fired after 4 parse-failed frames (K=3)\n");
    feed(fh, slabs, pktsB);                     // a clean B recovers via a geometry re-latch
    CHECK(ctx.got == csB, "hatch: clean B frame after the hatch not delivered clean\n");
    CHECK(!ctx.relatches.empty()
              && ctx.relatches.back() == j2k::frame_handler::kRelatchGeometry,
          "hatch: the clean B frame did not geometry-re-latch\n");
    CHECK(slabs.leaked() == 0, "hatch: %zu slabs leaked\n", slabs.leaked());
    scenarios++;
  }

  // ---- scenario 8: rate-only exemption — same geometry, different rate, NO re-latch ----
  if (argc >= 4) {
    std::vector<uint8_t> csC = read_file(argv[3]);
    if (csC.size() < 4 || csC[0] != 0xFF || csC[1] != 0x4F) {
      std::fprintf(stderr, "not a raw codestream: %s\n", argv[3]);
      return 2;
    }
    const std::vector<Pkt> pktsC = packetize(csC, 1400);
    if (pktsC.size() < 4) {
      std::fprintf(stderr, "stream C too small to packetize\n");
      return 2;
    }
    std::printf("stream %s: %zu B -> %zu packets\n", argv[3], csC.size(), pktsC.size());
    j2k::frame_handler fh;
    Ctx ctx;
    Slabs slabs;
    fh.set_release_slab_callback(&release_slab, &slabs);
    fh.set_chunk_callback(&on_chunk, &ctx);
    fh.set_frame_abort_callback(&on_abort, &ctx);
    fh.set_frame_ready_callback(&on_ready, &ctx);
    fh.set_stream_relatch_callback(&on_relatch, &ctx);
    feed(fh, slabs, pkts);
    feed(fh, slabs, pktsC);
    CHECK(ctx.got == csC, "rate-only: frame C not delivered clean\n");
    feed(fh, slabs, pkts);
    CHECK(ctx.got == cs, "rate-only: frame A after the rate flip not delivered clean\n");
    CHECK(ctx.relatches.empty(),
          "rate-only: %zu re-latches on a rate-only re-dial (the exemption is broken — "
          "geometry_signature must not hash rate-dependent bytes like SOT/Psot)\n",
          ctx.relatches.size());
    CHECK(ctx.aborts.empty() && ctx.frames_ready == 3 && ctx.frames_intact == 3,
          "rate-only: aborts=%zu ready=%zu intact=%zu != 0/3/3\n", ctx.aborts.size(),
          ctx.frames_ready, ctx.frames_intact);
    CHECK(slabs.leaked() == 0, "rate-only: %zu slabs leaked\n", slabs.leaked());
    scenarios++;
  }

  if (fails) {
    std::fprintf(stderr, "incremental_delivery_test: %d FAILURE(S)\n", fails);
    return 1;
  }
  std::printf("incremental_delivery_test: ALL PASS (%d scenarios)\n", scenarios);
  return 0;
}
