//
// Created by OSAMU WATANABE on 2024/11/21.
//

#include "j2k_tile.hpp"
#include "j2k_packet.hpp"
#include "utils.hpp"
#include <cassert>
#include <cstdlib>

void create_tile(tile_ *tile, siz_marker *siz, coc_marker *cocs, dfs_marker *dfs) {
  tile->progression_order = cocs[0].progression_order;
  for (uint32_t c = 0; c < tile->num_components; ++c) {
    tcomp_ *tcp     = &(tile->tcomp[c]);
    tcp->idx        = c;
    tcp->coord.x0   = ceildiv_int(tile->coord.x0, siz->XRsiz[c]);
    tcp->coord.y0   = ceildiv_int(tile->coord.y0, siz->YRsiz[c]);
    tcp->coord.x1   = ceildiv_int(tile->coord.x1, siz->XRsiz[c]);
    tcp->coord.y1   = ceildiv_int(tile->coord.y1, siz->YRsiz[c]);
    tcp->sub_x      = siz->XRsiz[c];
    tcp->sub_y      = siz->YRsiz[c];
    coc_marker *coc = &cocs[c];
    if (coc->NL > 32) {
      assert(dfs->idx == coc->NL - 128);
      coc->NL = dfs->Idfs;
    }
    parepare_tcomp_structure(tcp, siz, coc, dfs);
  }
  // read_tile(tile, cocs, dfs);
}

tile_ *create_tiles(siz_marker *siz, coc_marker *cocs, dfs_marker *dfs, codestream *buf) {
  const uint32_t numXtiles = ceildiv_int((siz->Xsiz - siz->XTOsiz), siz->XTsiz);
  const uint32_t numYtiles = ceildiv_int((siz->Ysiz - siz->YTOsiz), siz->YTsiz);
  // tile_ *tiles             = (tile_ *)calloc(numXtiles * numYtiles, sizeof(tile_));
  tile_ *tiles = NULL;
  if (numXtiles && numYtiles) tiles = (tile_ *)stackAlloc(numXtiles * numYtiles * sizeof(tile_), 0);
#ifdef DEBUG
  count_allocations(numXtiles * numYtiles * sizeof(tile_));
#endif
  for (uint32_t t = 0; t < numXtiles * numYtiles; ++t) {
    tile_ *tile          = &tiles[t];
    uint32_t p           = t % numXtiles;
    uint32_t q           = t / numXtiles;
    tile->buf            = buf;
    tile->idx            = t;
    tile->num_components = siz->Csiz;
    tile->coord.x0       = LOCAL_MAX(siz->XTOsiz + p * siz->XTsiz, siz->XOsiz);
    tile->coord.y0       = LOCAL_MAX(siz->YTOsiz + q * siz->YTsiz, siz->YOsiz);
    tile->coord.x1       = LOCAL_MIN(siz->XTOsiz + (p + 1) * siz->XTsiz, siz->Xsiz);
    tile->coord.y1       = LOCAL_MIN(siz->YTOsiz + (q + 1) * siz->YTsiz, siz->Ysiz);
    create_tile(tile, siz, cocs, dfs);
  }
  return tiles;
}

int read_tiles(tile_ *tiles, const siz_marker *siz, const coc_marker *cocs, const dfs_marker *dfs) {
  int ret;
  const uint32_t numXtiles = ceildiv_int((siz->Xsiz - siz->XTOsiz), siz->XTsiz);
  const uint32_t numYtiles = ceildiv_int((siz->Ysiz - siz->YTOsiz), siz->YTsiz);
  for (uint32_t t = 0; t < numXtiles * numYtiles; ++t) {
    ret = read_tile(&tiles[t], cocs, dfs);
    if (ret) {
      return ret;
    }
  }
  return EXIT_SUCCESS;
}

void restart_tiles(tile_ *tiles, const siz_marker *siz) {
  const uint32_t numXtiles = ceildiv_int((siz->Xsiz - siz->XTOsiz), siz->XTsiz);
  const uint32_t numYtiles = ceildiv_int((siz->Ysiz - siz->YTOsiz), siz->YTsiz);
  for (uint32_t t = 0; t < numXtiles * numYtiles; ++t) {
    tile_ *tile = &tiles[t];
    tile->buf->reset(0);
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

void destroy_tiles(tile_ *tiles, const siz_marker *siz) {
  // Currently, we use stack allocator. So, this function does nothing.

  const uint32_t numXtiles = ceildiv_int((siz->Xsiz - siz->XTOsiz), siz->XTsiz);
  const uint32_t numYtiles = ceildiv_int((siz->Ysiz - siz->YTOsiz), siz->YTsiz);
  for (uint32_t t = 0; t < numXtiles * numYtiles; ++t) {
    tile_ *tile = &tiles[t];
    for (uint32_t c = 0; c < tile->num_components; ++c) {
      tcomp_ *tcp = &(tile->tcomp[c]);
      for (uint32_t r = 0; r < MAX_DWT_LEVEL + 1; ++r) {
        res_ *res = &(tcp->res[r]);
        for (uint32_t p = 0; p < res->npw * res->nph; ++p) {
          prec_ *prec = &res->prec[p];
          for (uint32_t bp = 0; bp < prec->num_bands; ++bp) {
            pband_ *pband = &prec->pband[bp];
            // for (uint32_t n = 0; n < prec->ncbw * prec->ncbh; ++n) {
            //   free(&pband->blk[n]);
            // }
            free(pband->blk);
            free(pband->incl);
            free(pband->zbp);
          }
          free(prec->pband);
        }
        free(res->prec);
      }
    }
  }
  free(tiles);
}