#pragma once

#include <cassert>
#include <cstdint>
#include <vector>

#define MAX_NUM_COMPONENTS 3
#define MAX_DWT_LEVEL 5

extern uint32_t g_allocated_bytes;
void count_allocations(uint32_t n);
uint32_t get_bytes_allocated(void);

#define ceildiv_int(a, b) ((a) + ((b) - 1)) / (b)
#define LOCAL_MAX(a, b) ((a) > (b)) ? (a) : (b)
#define LOCAL_MIN(a, b) ((a) < (b)) ? (a) : (b)

class codestream {
private:
  const uint8_t *src;
  uint32_t pos;
  uint8_t tmp;
  uint8_t last;
  uint8_t bits;

public:
  explicit codestream(const uint8_t *buf) : src(buf), pos(0), tmp(0), last(0), bits(0) {
  }

  uint8_t get_byte();

  uint16_t get_word();

  uint32_t get_dword();

  int packetheader_get_bits(int n);

  void packetheader_flush_bits();

  void move_forward(uint32_t n);

  const uint8_t *get_address() const;

  uint32_t get_pos() const;

  void reset(uint32_t p);

};

inline uint8_t codestream::get_byte() {
  const uint8_t byte = src[pos];
  tmp     = 0;
  bits    = 0;
  pos++;
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

inline int codestream::packetheader_get_bits(int n) {
  int res = 0;

  while (--n >= 0) {
    res <<= 1;
    if (bits == 0) {
      last = tmp;
      tmp  = get_byte();
      bits = 7 + (last != 0xFFu);
    }
    bits--;
    res |= (tmp >> bits) & 1;
  }
  return res;
}

inline void codestream::packetheader_flush_bits() {
  if (tmp == 0xFFu) {
    pos++;
  }
  tmp  = 0;
  bits = 0;
}

inline void codestream::move_forward(uint32_t n) {
  assert(bits == 0);
  pos += n;
}

inline const uint8_t *codestream::get_address() const { return src + pos; }

inline uint32_t codestream::get_pos() const { return pos; }

inline void codestream::reset(uint32_t p) {
  last = 0;
  bits = 0;
  tmp  = 0;
  pos  = p;
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
  uint8_t type[MAX_DWT_LEVEL + 1];      // = 1: BOTH, 2: HORZ, 3: VERT
  uint8_t h_orient[MAX_DWT_LEVEL + 1];  // = 1 means X, 0 means L or H, (XL or XH)
  uint8_t v_orient[MAX_DWT_LEVEL + 1];  // = 1 means X, 0 means L or H, (LX or HX)
  uint8_t hor_depth[MAX_DWT_LEVEL + 1];
  uint8_t ver_depth[MAX_DWT_LEVEL + 1];
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
  uint8_t val;
  uint8_t temp_val;
  uint8_t vis;
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

  tile_(uint32_t t, uint32_t po) {
    buf = nullptr;
    idx = t;
    coord = {};
    num_components = MAX_NUM_COMPONENTS;
    progression_order = po;
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