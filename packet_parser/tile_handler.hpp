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
        ready(false) {}
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
          parepare_tcomp_structure(tcp, &siz, coc, &dfs);
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

    for (int32_t i = PID - tile->crp_idx; i > 16; --i) {
      ret = parse_one_precinct(tile, this->cocs);
      tile->crp_idx++;
      if (ret) {
        break;
      }
    }
    // } // This loop is omitted for speed
    return ret;
  }

  int flush() {
    int ret     = EXIT_SUCCESS;
    tile_ *tile = tiles.data();
    for (; tile->crp_idx < 5670; tile->crp_idx++) {
      ret = parse_one_precinct(tile, this->cocs);
      if (ret) {
        break;
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
      for (uint32_t c = 0; c < tile->num_components; ++c) {
        tcomp_ *tcp = &(tile->tcomp[c]);
        for (uint32_t r = 0; r < MAX_DWT_LEVEL + 1; ++r) {
          res_ *res = &(tcp->res[r]);
          for (uint32_t p = 0; p < res->npw * res->nph; ++p) {
            prec_ *prec = &res->prec[p];
            for (uint32_t bp = 0; bp < prec->num_bands; ++bp) {
              pband_ *pband = &prec->pband[bp];
              tag_tree_zero(pband->incl, prec->ncbw, prec->ncbh, 0);
              tag_tree_zero(pband->zbp, prec->ncbw, prec->ncbh, 0);
              for (uint32_t n = 0; n < prec->ncbw * prec->ncbh; ++n) {
                blk_ *blk            = &pband->blk[n];
                blk->length          = 0;
                blk->incl            = 0;
                blk->zbp             = 0;
                blk->lblock          = 3;
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
