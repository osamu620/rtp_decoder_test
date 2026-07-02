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
// usage: incremental_delivery_test <stream.j2c>   (exit 0 pass / 1 fail / 2 bad input)
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

  if (fails) {
    std::fprintf(stderr, "incremental_delivery_test: %d FAILURE(S)\n", fails);
    return 1;
  }
  std::printf("incremental_delivery_test: ALL PASS (4 scenarios)\n");
  return 0;
}
