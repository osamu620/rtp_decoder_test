//
// Created by OSAMU WATANABE on 2024/12/04.
//

#ifndef TILE_HANDLER_HPP
#define TILE_HANDLER_HPP

#include <cstdint>
#include <cassert>
#include <vector>
#include "j2k_packet.hpp"
#include "utils.hpp"

class tile_hanlder {
 public:
  // Fired after each successful parse_one_precinct(), in PID order, on the parser thread.
  // Keep work minimal — long callbacks defeat the sub-codestream-latency goal.
  using PrecinctReadyCb = void (*)(void *user, const prec_ *pp, uint8_t c, uint8_t r, uint16_t p);

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
  uint32_t parse_holdback_;

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
        parse_holdback_(16) {}

  void set_precinct_callback(PrecinctReadyCb cb, void *arg) {
    prec_cb_     = cb;
    prec_cb_arg_ = arg;
  }

  void set_parse_holdback(uint32_t n) { parse_holdback_ = n; }
  uint32_t get_parse_holdback() const { return parse_holdback_; }
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

  int parse(uint32_t PID) {
    int ret = EXIT_SUCCESS;
    tile_ *tile;
    // This loop is omitted for speed
    // for (uint32_t t = 0; t < this->num_tiles_x * this->num_tiles_y; ++t) {
    tile = tiles.data();  // + t;

    // Detail information of PID
    // uint32_t s = PID / 3;
    // uint32_t c = PID % 3;

    const int32_t holdback = static_cast<int32_t>(parse_holdback_);
    for (int32_t i = PID - tile->crp_idx; i > holdback; --i) {
      const crp_status ct = tile->crp[tile->crp_idx];
      ret                 = parse_one_precinct(tile, this->cocs);
      tile->crp_idx++;
      if (ret) {
        break;
      }
      if (prec_cb_) {
        const prec_ *pp = &tile->tcomp[ct.c].res[ct.r].prec[ct.p];
        prec_cb_(prec_cb_arg_, pp, ct.c, ct.r, ct.p);
      }
    }
    // } // This loop is omitted for speed
    return ret;
  }

  int flush() {
    int ret     = EXIT_SUCCESS;
    tile_ *tile = tiles.data();
    const int n = static_cast<int>(tile->crp.size());
    for (; tile->crp_idx < n; tile->crp_idx++) {
      const crp_status ct = tile->crp[tile->crp_idx];
      ret                 = parse_one_precinct(tile, this->cocs);
      if (ret) {
        break;
      }
      if (prec_cb_) {
        const prec_ *pp = &tile->tcomp[ct.c].res[ct.r].prec[ct.p];
        prec_cb_(prec_cb_arg_, pp, ct.c, ct.r, ct.p);
      }
    }
    return ret;
  }

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
