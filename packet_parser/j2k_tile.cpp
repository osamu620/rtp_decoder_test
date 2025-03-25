// //
// // Created by OSAMU WATANABE on 2024/11/21.
// //
//
// #include "j2k_tile.hpp"
// #include "j2k_packet.hpp"
// #include "utils.hpp"
// #include <cassert>
// #include <cstdlib>
//
// void restart_tiles(tile_ *tiles, const siz_marker *siz) {
//   const uint32_t numXtiles = ceildiv_int((siz->Xsiz - siz->XTOsiz), siz->XTsiz);
//   const uint32_t numYtiles = ceildiv_int((siz->Ysiz - siz->YTOsiz), siz->YTsiz);
//   for (uint32_t t = 0; t < numXtiles * numYtiles; ++t) {
//     tile_ *tile = &tiles[t];
//     tile->buf->reset(0);
//     for (uint32_t c = 0; c < tile->num_components; ++c) {
//       tcomp_ *tcp = &(tile->tcomp[c]);
//       for (uint32_t r = 0; r < MAX_DWT_LEVEL + 1; ++r) {
//         res_ *res                   = &(tcp->res[r]);
//         const int32_t num_precincts = (int32_t)(res->npw * res->nph);
//         for (int32_t p = num_precincts - 1; p >= 0; --p) {
//           prec_ *prec          = &res->prec[p];
//           const auto num_cblks = (int32_t)(prec->ncbw * prec->ncbh);
//           for (int32_t bp = prec->num_bands - 1; bp >= 0; --bp) {
//             pband_ *pband = &prec->pband[bp];
//             tag_tree_zero(pband->incl, prec->ncbw, prec->ncbh, 0);
//             tag_tree_zero(pband->zbp, prec->ncbw, prec->ncbh, 0);
//             for (int32_t n = num_cblks - 1; n >= 0; --n) {
//               blk_ *blk            = &pband->blk[n];
//               blk->length          = 0;
//               blk->incl            = 0;
//               blk->zbp             = 0;
//               blk->lblock          = 3;
//               blk->npasses         = 0;
//               blk->pass_lengths[0] = blk->pass_lengths[1] = 0;
//             }
//           }
//         }
//       }
//     }
//   }
//   stackAlloc(0, 1);
// }
//
// void destroy_tiles(tile_ *tiles, const siz_marker *siz) {
//   // Currently, we use stack allocator. So, this function does nothing.
//
//   const uint32_t numXtiles = ceildiv_int((siz->Xsiz - siz->XTOsiz), siz->XTsiz);
//   const uint32_t numYtiles = ceildiv_int((siz->Ysiz - siz->YTOsiz), siz->YTsiz);
//   for (uint32_t t = 0; t < numXtiles * numYtiles; ++t) {
//     tile_ *tile = &tiles[t];
//     for (uint32_t c = 0; c < tile->num_components; ++c) {
//       tcomp_ *tcp = &(tile->tcomp[c]);
//       for (uint32_t r = 0; r < MAX_DWT_LEVEL + 1; ++r) {
//         res_ *res = &(tcp->res[r]);
//         for (uint32_t p = 0; p < res->npw * res->nph; ++p) {
//           prec_ *prec = &res->prec[p];
//           for (uint32_t bp = 0; bp < prec->num_bands; ++bp) {
//             pband_ *pband = &prec->pband[bp];
//             // for (uint32_t n = 0; n < prec->ncbw * prec->ncbh; ++n) {
//             //   free(&pband->blk[n]);
//             // }
//             free(pband->blk);
//             free(pband->incl);
//             free(pband->zbp);
//           }
//           free(prec->pband);
//         }
//         free(res->prec);
//       }
//     }
//   }
//   free(tiles);
// }