#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

#define MAX_NUM_COMPONENTS 3
#define MAX_DWT_LEVEL 5

extern uint32_t g_allocated_bytes;
void count_allocations(uint32_t n);
uint32_t get_bytes_allocated(void);

#define ceildiv_int(a, b) ((a) + ((b) - 1)) / (b)
#define LOCAL_MAX(a, b) ((a) > (b)) ? (a) : (b)
#define LOCAL_MIN(a, b) ((a) < (b)) ? (a) : (b)

// Chain-reader codestream. Bytes flow through an ordered list of (base, len) chunks
// appended by frame_handler as RTP packets arrive — no copy into a contiguous buffer.
// Reads automatically transition across chunks. For codeblock body access where the
// FPGA driver needs a contiguous pointer, take_contiguous() returns either a direct
// pointer (when the body fits in one chunk) or a heap-allocated staging copy.
//
// IMPORTANT: staging buffers must NOT come from stackAlloc — that arena is shared with
// the per-stream tile/precinct/pband/blk/tagtree structures, and tile_handler::restart
// resets its index every frame. A staging allocation from the same arena would overwrite
// those structures. We use std::vector<uint8_t> per spanning codeblock instead, kept
// alive until codestream::clear() (called at frame restart).

class codestream {
 private:
  struct Chunk {
    const uint8_t *base;
    size_t len;
  };
  std::vector<Chunk> chunks_;
  // Per-frame staging buffers for codeblock bodies that span chunk boundaries. Each
  // entry is owned for the lifetime of the chain (cleared on clear()).
  std::vector<std::vector<uint8_t>> staging_;
  size_t cur_chunk_                = 0;
  size_t cur_offset_               = 0;
  size_t consumed_before_cur_chunk_ = 0;
  uint8_t tmp  = 0;
  uint8_t last = 0;
  uint8_t bits = 0;
#ifdef PARSER_OVERSHOOT_INSTR
  size_t max_offset_read_ = 0;
#endif

  // Walk cur_chunk_/cur_offset_ forward by n bytes.
  void advance(size_t n) {
    cur_offset_ += n;
    while (cur_chunk_ < chunks_.size() && cur_offset_ >= chunks_[cur_chunk_].len) {
      cur_offset_ -= chunks_[cur_chunk_].len;
      consumed_before_cur_chunk_ += chunks_[cur_chunk_].len;
      cur_chunk_++;
    }
  }

 public:
  codestream() = default;
  // Backward-compat constructor: takes a single buffer pointer (length unspecified).
  // Caller must call append_chunk to provide the actual length. Kept only so the
  // existing frame_handler initializer-list `cs(incoming_data)` compiles during the
  // transition; frame_handler will be updated to use append_chunk explicitly.
  explicit codestream(uint8_t *) {}

#ifdef PARSER_OVERSHOOT_INSTR
  size_t get_max_offset_read() const { return max_offset_read_; }
  void reset_max_offset_read() { max_offset_read_ = consumed_before_cur_chunk_ + cur_offset_; }
  void note_external_read(const uint8_t *addr) {
    // addr is a pointer into one of the chunks; find which one and compute absolute offset.
    for (size_t i = 0; i < chunks_.size(); ++i) {
      const uint8_t *base = chunks_[i].base;
      if (addr >= base && addr < base + chunks_[i].len) {
        size_t off = 0;
        for (size_t j = 0; j < i; ++j) off += chunks_[j].len;
        off += static_cast<size_t>(addr - base);
        if (off > max_offset_read_) max_offset_read_ = off;
        return;
      }
    }
    // Pointer doesn't belong to any chunk — likely a staging buffer; ignore.
  }
#else
  void note_external_read(const uint8_t *) {}
#endif

  // Chain management.
  void append_chunk(const uint8_t *base, size_t len) { chunks_.push_back({base, len}); }
  void clear() {
    chunks_.clear();
    staging_.clear();
    cur_chunk_                 = 0;
    cur_offset_                = 0;
    consumed_before_cur_chunk_ = 0;
    tmp                        = 0;
    last                       = 0;
    bits                       = 0;
#ifdef PARSER_OVERSHOOT_INSTR
    max_offset_read_ = 0;
#endif
  }

  uint8_t get_byte();
  uint16_t get_word();
  uint32_t get_dword();
  uint32_t get_bit();
  int packetheader_get_bits(int n);
  void packetheader_flush_bits();
  void move_forward(uint32_t n);
  const uint8_t *get_address() const;
  uint32_t get_pos() const;
  void reset(uint32_t p);
  // Returns a pointer to len contiguous bytes starting at current position; advances by
  // len. If the bytes fit within the current chunk, the returned pointer is a direct
  // chunk pointer. Otherwise, copies the bytes into a per-frame heap-owned staging
  // buffer (held alive in staging_, freed on clear()). Used by codeblock body access
  // where downstream consumers need a contiguous pointer.
  const uint8_t *take_contiguous(size_t len);
};

inline uint8_t codestream::get_byte() {
  // Bounds check: parser drift (e.g., a wrong cblk->length) can drive cur_chunk_ past
  // chunks_.size(). Returning 0 lets the parser fail a validation check cleanly
  // instead of segfaulting; the frame will be marked truncated and recovery proceeds
  // at EOC or the next held_slabs cap fire.
  if (cur_chunk_ >= chunks_.size()) return 0;
  const uint8_t byte = chunks_[cur_chunk_].base[cur_offset_++];
  if (cur_offset_ == chunks_[cur_chunk_].len) {
    consumed_before_cur_chunk_ += chunks_[cur_chunk_].len;
    cur_chunk_++;
    cur_offset_ = 0;
  }
#ifdef PARSER_OVERSHOOT_INSTR
  const size_t off = consumed_before_cur_chunk_ + cur_offset_;
  if (off > 0 && off - 1 > max_offset_read_) max_offset_read_ = off - 1;
#endif
  return byte;
}

inline uint16_t codestream::get_word() {
  uint16_t word = get_byte();
  word <<= 8;
  word |= get_byte();
  return word;
}

inline uint32_t codestream::get_dword() {
  uint32_t dword = get_word();
  dword <<= 16;
  dword |= get_word();
  return dword;
}

inline uint32_t codestream::get_bit() {
  if (bits == 0) {
    bits = (tmp == 0xFF) ? 7 : 8;
    last = tmp;
    tmp  = get_byte();
  }
  bits--;
  return (tmp >> bits) & 1;
}

inline int codestream::packetheader_get_bits(int n) {
  int res = 0;
  while (--n >= 0) {
    res <<= 1;
    if (bits == 0) {
      bits = (tmp == 0xFF) ? 7 : 8;
      last = tmp;
      tmp  = get_byte();
    }
    bits--;
    res |= (tmp >> bits) & 1;
  }
  return res;
}

inline void codestream::packetheader_flush_bits() {
  if (tmp == 0xFFu) advance(1);
  tmp  = 0;
  bits = 0;
}

inline void codestream::move_forward(uint32_t n) {
  assert(bits == 0);
  advance(n);
}

inline const uint8_t *codestream::get_address() const {
  // Bounds-safe: returns nullptr if cur_chunk_ has walked past the end. Callers that
  // dereference must check (none currently do unconditionally — take_contiguous is
  // used for codeblock body access, which has its own safety).
  if (cur_chunk_ >= chunks_.size()) return nullptr;
  return chunks_[cur_chunk_].base + cur_offset_;
}

inline uint32_t codestream::get_pos() const {
  return static_cast<uint32_t>(consumed_before_cur_chunk_ + cur_offset_);
}

inline void codestream::reset(uint32_t p) {
  last = 0;
  bits = 0;
  tmp  = 0;
  // Walk chunks to find the one containing absolute position p. If p is past the end
  // of the chain, leave cur_chunk_ at chunks_.size() (out of range); subsequent reads
  // will return 0 via the bounds check in get_byte.
  cur_chunk_                 = 0;
  cur_offset_                = 0;
  consumed_before_cur_chunk_ = 0;
  size_t remaining = p;
  while (cur_chunk_ < chunks_.size() && remaining >= chunks_[cur_chunk_].len) {
    remaining -= chunks_[cur_chunk_].len;
    consumed_before_cur_chunk_ += chunks_[cur_chunk_].len;
    cur_chunk_++;
  }
  cur_offset_ = remaining;
}

inline const uint8_t *codestream::take_contiguous(size_t len) {
  // Bounds-safe variant: if cur_chunk_ has walked past the end (parser drift) or len
  // exceeds remaining bytes, return nullptr. Callers (codeblock body access) check for
  // a non-null pointer before dereferencing.
  if (cur_chunk_ >= chunks_.size()) return nullptr;
  if (len == 0) return chunks_[cur_chunk_].base + cur_offset_;
  // Fast path: fits in current chunk.
  if (cur_offset_ + len <= chunks_[cur_chunk_].len) {
    const uint8_t *result = chunks_[cur_chunk_].base + cur_offset_;
    advance(len);
    return result;
  }
  // Slow path: spans chunks; copy into a per-frame staging buffer. Use a fresh
  // std::vector so each spanning codeblock has its own contiguous storage that lives
  // until clear() (= frame restart). Do NOT use stackAlloc here: the static arena is
  // shared with per-stream tile/precinct/pband/blk/tagtree structures and would
  // overwrite them after restart() resets the index.
  staging_.emplace_back(len);
  uint8_t *staging = staging_.back().data();
  size_t copied = 0;
  while (copied < len && cur_chunk_ < chunks_.size()) {
    size_t avail   = chunks_[cur_chunk_].len - cur_offset_;
    size_t to_copy = (len - copied < avail) ? len - copied : avail;
    std::memcpy(staging + copied, chunks_[cur_chunk_].base + cur_offset_, to_copy);
    copied += to_copy;
    advance(to_copy);
  }
  return staging;
}

// typedef struct {
//   uint32_t MainRESET;
//   uint32_t PageRESET;
//   uint32_t PageEND_PageStart;
//   uint32_t Cb_Image_WH;
//   uint32_t ResLevel_Porder;
//   uint32_t CB_indicator;
//   uint32_t CB_startAddr0;
//   uint32_t MELOffset_CodeLength0;
//   uint32_t Cb_startAddr1;
//   uint32_t MELOffset_CodeLength1;
//   uint32_t CbS_startAddr2;
//   uint32_t Qsomething;
//   uint32_t Qvalue0;
//   uint32_t Qvalue1;
//   uint32_t XTsiz;
//   uint32_t YTsiz;
//   uint32_t PPx[32];
//   uint32_t PPy[32];
// } IO_map;

struct siz_marker {
  uint16_t Rsiz;
  uint32_t Xsiz;
  uint32_t Ysiz;
  uint32_t XOsiz;
  uint32_t YOsiz;
  uint32_t XTsiz;
  uint32_t YTsiz;
  uint32_t XTOsiz;
  uint32_t YTOsiz;
  uint16_t Csiz;
  uint8_t Ssiz[MAX_NUM_COMPONENTS];
  uint8_t XRsiz[MAX_NUM_COMPONENTS];
  uint8_t YRsiz[MAX_NUM_COMPONENTS];
};

struct cod_marker {
  uint8_t max_precinct;
  uint8_t use_SOP;
  uint8_t use_EPH;
  uint8_t use_h_offset;
  uint8_t use_v_offset;
  uint8_t use_blkcoder_extension;
  uint8_t progression_order;
  uint16_t num_layers;
  uint8_t mct;
  uint8_t NL;         // number of DWT levels
  uint8_t cbw;        // code-block width
  uint8_t cbh;        // code-block height
  uint8_t cbs;        // code-block style
  uint8_t transform;  // type of wavelet transform
  uint16_t SSO;
  uint8_t prw[MAX_DWT_LEVEL + 1];
  uint8_t prh[MAX_DWT_LEVEL + 1];
};

struct qcd_marker {
  uint8_t type;
  uint8_t num_guard_bits;
  uint8_t exponent[MAX_DWT_LEVEL * 3 + 1];   // for HORZ and VERT, we skip 2 items
  uint16_t mantissa[MAX_DWT_LEVEL * 3 + 1];  // for HORZ and VERT, we skip 2 items
};

struct coc_marker {
  uint8_t idx;
  uint8_t max_precinct;
  uint8_t use_SOP;
  uint8_t use_EPH;
  uint8_t use_h_offset;
  uint8_t use_v_offset;
  uint8_t use_blkcoder_extension;
  uint8_t progression_order;
  uint16_t num_layers;
  uint8_t mct;
  uint8_t NL;         // number of DWT levels
  uint8_t cbw;        // code-block width
  uint8_t cbh;        // code-block height
  uint8_t cbs;        // code-block style
  uint8_t transform;  // type of wavelet transform
  uint16_t SSO;
  uint8_t prw[MAX_DWT_LEVEL + 1];
  uint8_t prh[MAX_DWT_LEVEL + 1];
  int32_t step_x;
  int32_t step_y;
};

struct dfs_marker {
  uint32_t idx;
  uint32_t Idfs;
  uint32_t type[MAX_DWT_LEVEL + 1];      // = 1: BOTH, 2: HORZ, 3: VERT
  uint32_t h_orient[MAX_DWT_LEVEL + 1];  // = 1 means X, 0 means L or H, (XL or XH)
  uint32_t v_orient[MAX_DWT_LEVEL + 1];  // = 1 means X, 0 means L or H, (LX or HX)
  uint32_t hor_depth[MAX_DWT_LEVEL + 1];
  uint32_t ver_depth[MAX_DWT_LEVEL + 1];
};

struct geometry {
  int32_t x0;
  int32_t y0;
  int32_t x1;
  int32_t y1;
};

struct blk_ {
  // struct band_ *parent_band;
  // struct prec_ *parent_prec;
  geometry coord;
  uint8_t *data;
  uint32_t pass_lengths[2];
  uint32_t length;
  uint32_t Scup;  // beginning of MEL stream
  uint16_t modes;
  uint8_t incl;
  uint8_t lblock;
  uint8_t zbp;
  uint8_t npasses;
  uint8_t ht_plhd;
  // uint8_t nb_lengthinc;
  // uint8_t nb_terminationsinc;
  // uint8_t nb_terminations;
};

typedef struct tagtree_node tagtree_node;

struct tagtree_node {
  uint32_t val;
  uint32_t temp_val;
  uint32_t vis;
  tagtree_node *parent;
};

struct pband_ {
  tagtree_node *incl;
  tagtree_node *zbp;
  blk_ *blk;
};

struct prec_ {
  pband_ *pband;
  geometry coord;
  uint32_t res_num;
  uint32_t num_bands;
  uint32_t ncbw;
  uint32_t ncbh;
  uint8_t use_SOP;
  uint8_t use_EPH;
};

struct band_ {
  geometry coord;
};

struct res_ {
  band_ band[3];
  prec_ *prec;
  uint32_t idx;
  geometry coord;
  uint32_t npw;
  uint32_t nph;
  uint32_t transform_direction;
  uint8_t num_bands;
};

struct tcomp_ {
  res_ res[MAX_DWT_LEVEL + 1];
  uint32_t idx;
  geometry coord;
  uint32_t sub_x;
  uint32_t sub_y;
};

struct crp_status {
  uint8_t c;
  uint8_t r;
  uint16_t p;
};

class tile_ {
 public:
  tcomp_ tcomp[MAX_NUM_COMPONENTS];
  codestream *buf;
  uint32_t idx;
  geometry coord;
  uint32_t num_components;
  uint32_t progression_order;
  std::vector<crp_status> crp;
  int crp_idx;
  bool is_crp_complete;

  tile_(uint32_t t, uint32_t po) {
    buf               = nullptr;
    idx               = t;
    coord             = {};
    num_components    = MAX_NUM_COMPONENTS;
    progression_order = po;
    crp.resize(1890 * 3);
    crp_idx         = 0;
    is_crp_complete = false;
  }
};

enum DWT_direction { BOTH = 1, HORZ, VERT };

enum j2kmarker {
  // Delimiting markers and marker segments
  SOC = 0xFF4F,
  SOT = 0xFF90,
  SOD = 0xFF93,
  EOC = 0xFFD9,

  // Fixed information marker segments
  SIZ = 0xFF51,
  PRF = 0xFF56,
  CAP = 0xFF50,

  // Functional marker segments
  COD = 0xFF52,
  COC = 0xFF53,
  RGN = 0xFF5E,
  QCD = 0xFF5C,
  QCC = 0xFF5D,
  POC = 0xFF5F,

  // Pointer marker segments
  TLM = 0xFF55,
  PLM = 0xFF57,
  PLT = 0xFF58,
  PPM = 0xFF60,
  PPT = 0xFF61,

  // In-bit-stream markers and marker segments
  SOP = 0xFF91,
  EPH = 0xFF92,

  // Informational marker segments
  CRG = 0xFF63,
  COM = 0xFF64,

  // Part 2 marker segment
  DFS = 0xFF72,

  // Part 15 marker segment
  CPF = 0xFF59
};

enum porder { LRCP, RLCP, RPCL, PCRL, CPRL, PRCL };