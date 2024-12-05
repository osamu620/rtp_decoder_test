//
// Created by OSAMU WATANABE on 2024/11/21.
//

#include <cstdio>
#include <cstdlib>

#include "j2k_packet.hpp"

#include <cassert>

#include "utils.hpp"

extern FILE *log_file;

#define JPEG2000_MAX_PASSES 100
#define CMODE_BYPASS 0x01    // Selective arithmetic coding bypass
#define CMODE_TERMALL 0x04   // Terminate after each coding pass
#define CMODE_HT 0x40        // Only HT code-blocks (Rec. ITU-T T.814 | ISO/IEC 15444-15) are  present
#define CMODE_HT_MIXED 0xC0  // HT code-blocks (Rec. ITU-T T.814 | ISO/IEC 15444-15) can be present
#define HT_MIXED 0x80        // bit 7 of SPcod/SPcoc

// Values of flag for placeholder passes
enum HT_PLHD_STATUS { HT_PLHD_OFF, HT_PLHD_ON };

static int tag_tree_decode(codestream *buf, tagtree_node *node, int threshold) {
  tagtree_node *stack[30];
  int sp = -1, curval = 0;

  if (!node) {
    return 1;
  }

  while (node && !node->vis) {
    stack[++sp] = node;
    node        = node->parent;
  }

  if (node)
    curval = node->val;
  else
    curval = stack[sp]->val;

  while (curval < threshold && sp >= 0) {
    if (curval < stack[sp]->val) curval = stack[sp]->val;
    while (curval < threshold) {
      int ret;
      if ((ret = buf->packetheader_get_bits(1)) > 0) {
        stack[sp]->vis++;
        break;
      } else if (!ret)
        curval++;
      else
        return ret;
    }
    stack[sp]->val = curval;
    sp--;
  }
  return curval;
}

static int32_t tag_tree_size(int w, int h) {
  int64_t res = 0;
  while (w > 1 || h > 1) {
    res += w * (int64_t)h;
    assert(res + 1 < INT32_MAX);
    w = (w + 1) >> 1;
    h = (h + 1) >> 1;
  }
  return (int32_t)(res + 1);
}

/* allocate the memory for tag tree */
static tagtree_node *tag_tree_init(int w, int h) {
  int pw = w, ph = h;
  tagtree_node *res, *t, *t2;
  int32_t tt_size;

  tt_size = tag_tree_size(w, h);

  // t = res = calloc(tt_size, sizeof(*t));
  t = res = (tagtree_node *)stackAlloc(tt_size * sizeof(*t), 0);
#ifdef DEBUG
  count_allocations(tt_size * sizeof(*t));
#endif

  if (!res) return NULL;

  while (w > 1 || h > 1) {
    int i, j;
    pw = w;
    ph = h;

    w  = (w + 1) >> 1;
    h  = (h + 1) >> 1;
    t2 = t + pw * ph;

    for (i = 0; i < ph; i++)
      for (j = 0; j < pw; j++) t[i * pw + j].parent = &t2[(i >> 1) * w + (j >> 1)];

    t = t2;
  }
  t[0].parent = NULL;
  return res;
}

void tag_tree_zero(tagtree_node *t, int w, int h, int val) {
  int i, siz = tag_tree_size(w, h);

  for (i = 0; i < siz; i++) {
    t[i].val      = val;
    t[i].temp_val = 0;
    t[i].vis      = 0;
  }
}

void parepare_tcomp_structure(tcomp_ *tcp, siz_marker *siz, coc_marker *coc, dfs_marker *dfs) {
  const uint32_t xob[4] = {0, 1, 0, 1};
  const uint32_t yob[4] = {0, 0, 1, 1};
  const uint32_t cbw    = 1 << coc->cbw;
  const uint32_t cbh    = 1 << coc->cbh;
  const uint32_t NL     = coc->NL;
  // resolution
  for (int32_t r = (int32_t)NL; r >= 0; --r) {
    res_ *rp                = &(tcp->res[r]);
    rp->idx                 = r;
    rp->transform_direction = dfs->type[NL - r + 1];
    rp->num_bands           = 3;
    if (rp->transform_direction != BOTH || r == 0) {
      // only `Both` type has 3 subbands
      rp->num_bands = 1;
    }

    rp->coord.x0 = ceildiv_int(tcp->coord.x0, 1 << dfs->hor_depth[NL - r]);
    rp->coord.y0 = ceildiv_int(tcp->coord.y0, 1 << dfs->ver_depth[NL - r]);
    rp->coord.x1 = ceildiv_int(tcp->coord.x1, 1 << dfs->hor_depth[NL - r]);
    rp->coord.y1 = ceildiv_int(tcp->coord.y1, 1 << dfs->ver_depth[NL - r]);

    rp->npw = (rp->coord.x1 > rp->coord.x0)
                  ? ceildiv_int(rp->coord.x1, 1 << coc->prw[r]) - rp->coord.x0 / (1 << coc->prw[r])
                  : 0;
    rp->nph = (rp->coord.y1 > rp->coord.y0)
                  ? ceildiv_int(rp->coord.y1, 1 << coc->prh[r]) - rp->coord.y0 / (1 << coc->prh[r])
                  : 0;
    // subband
    for (uint32_t b = 0; b < rp->num_bands; ++b) {
      band_ *bp = &(rp->band[b]);

      uint32_t nb = (rp->idx == 0) ? NL - rp->idx : NL - rp->idx + 1;

      uint32_t nb_1 = 0;

      if (nb > 0) {
        nb_1 = nb - 1;
      }
      uint32_t i = (rp->num_bands == 1) ? 0 : b + 1;
      if (rp->transform_direction == HORZ) {
        bp->coord.x0 = (rp->coord.x0 + 1) >> 1;
        bp->coord.y0 = rp->coord.y0;
        bp->coord.x1 = (rp->coord.x1 + 1) >> 1;
        bp->coord.y1 = rp->coord.y1;
      } else if (rp->transform_direction == VERT) {
        bp->coord.x0 = rp->coord.x0;
        bp->coord.y0 = (rp->coord.y0 + 1) >> 1;
        bp->coord.x1 = rp->coord.x1;
        bp->coord.y1 = (rp->coord.y1 + 1) >> 1;
      } else if (rp->transform_direction == BOTH) {
        bp->coord.x0 = ceildiv_int((tcp->coord.x0 - (1 << (nb_1)) * xob[i]), 1 << nb);
        bp->coord.y0 = ceildiv_int((tcp->coord.y0 - (1 << (nb_1)) * yob[i]), 1 << nb);
        bp->coord.x1 = ceildiv_int((tcp->coord.x1 - (1 << (nb_1)) * xob[i]), 1 << nb);
        bp->coord.y1 = ceildiv_int((tcp->coord.y1 - (1 << (nb_1)) * yob[i]), 1 << nb);
      } else {
        // should be r == 0
        if ((rp + 1)->transform_direction != BOTH) {
          bp->coord.x0 = (rp + 1)->coord.x0;
          bp->coord.y0 = (rp + 1)->coord.y0;
          bp->coord.x1 = ((rp + 1)->transform_direction == HORZ) ? (rp + 1)->coord.x1 - rp->coord.x1
                                                                 : (rp + 1)->coord.x1;
          bp->coord.y1 = ((rp + 1)->transform_direction == VERT) ? (rp + 1)->coord.y1 - rp->coord.y1
                                                                 : (rp + 1)->coord.y1;
        } else {
          bp->coord.x0 = ceildiv_int((tcp->coord.x0 - (1 << (nb_1)) * xob[i]), 1 << nb);
          bp->coord.y0 = ceildiv_int((tcp->coord.y0 - (1 << (nb_1)) * yob[i]), 1 << nb);
          bp->coord.x1 = ceildiv_int((tcp->coord.x1 - (1 << (nb_1)) * xob[i]), 1 << nb);
          bp->coord.y1 = ceildiv_int((tcp->coord.y1 - (1 << (nb_1)) * yob[i]), 1 << nb);
        }
      }
    }
    // precinct
    // rp->prec = (prec_ *)malloc(sizeof(prec_) * rp->npw * rp->nph);
    rp->prec = (prec_ *)stackAlloc(sizeof(prec_) * rp->npw * rp->nph, 0);
#ifdef DEBUG
    count_allocations(sizeof(prec_) * rp->npw * rp->nph);
#endif
    for (uint32_t np = 0; np < rp->npw * rp->nph; ++np) {
      uint32_t ppx   = 1 << coc->prw[r];
      uint32_t ppy   = 1 << coc->prh[r];
      prec_ *precp   = &(rp->prec[np]);
      precp->res_num = r;

      uint32_t x      = np % rp->npw;
      uint32_t y      = np / rp->npw;
      precp->coord.x0 = LOCAL_MAX(rp->coord.x0, ppx * (x + (rp->coord.x0 / ppx)));
      precp->coord.y0 = LOCAL_MAX(rp->coord.y0, ppy * (y + (rp->coord.y0 / ppy)));
      precp->coord.x1 = LOCAL_MIN(rp->coord.x1, ppx * (x + 1 + (rp->coord.x0 / ppx)));
      precp->coord.y1 = LOCAL_MIN(rp->coord.y1, ppy * (y + 1 + (rp->coord.y0 / ppy)));

      precp->num_bands = rp->num_bands;
      precp->use_EPH   = coc->use_EPH;
      precp->use_SOP   = coc->use_SOP;

      // precp->pband = (pband_ *)malloc(sizeof(pband_) * precp->num_bands);
      precp->pband = (pband_ *)stackAlloc(sizeof(pband_) * precp->num_bands, 0);
#ifdef DEBUG
      count_allocations(sizeof(pband_) * precp->num_bands);
#endif
      for (uint32_t b = 0; b < rp->num_bands; ++b) {
        pband_ *pband    = &(precp->pband[b]);
        const uint32_t i = (rp->num_bands == 1) ? 0 : b + 1;
        // uint32_t sr   = (rp->num_bands == 1) ? 1 : 2;
        uint32_t srx, sry;
        switch (rp->transform_direction) {
          case BOTH:
            srx = sry = 2;
            break;
          case HORZ:
            srx = 2;
            sry = 1;
            break;
          case VERT:
            srx = 1;
            sry = 2;
            break;
          default:
            srx = 1;
            sry = 1;
            break;
        }
        uint32_t pbx0 = ceildiv_int(precp->coord.x0 - xob[i], srx);
        uint32_t pbx1 = ceildiv_int(precp->coord.x1 - xob[i], srx);
        uint32_t pby0 = ceildiv_int(precp->coord.y0 - yob[i], sry);
        uint32_t pby1 = ceildiv_int(precp->coord.y1 - yob[i], sry);

        precp->ncbw = 0;
        if (pbx1 > pbx0) {
          precp->ncbw = ceildiv_int(pbx1, cbw) - pbx0 / cbw;
        }
        precp->ncbh = 0;
        if (pby1 > pby0) {
          precp->ncbh = ceildiv_int(pby1, cbh) - pby0 / cbh;
        }
        if (precp->ncbw && precp->ncbh) {
          pband->incl = tag_tree_init(precp->ncbw, precp->ncbh);
          pband->zbp  = tag_tree_init(precp->ncbw, precp->ncbh);
          tag_tree_zero(pband->incl, precp->ncbw, precp->ncbh, 0);
          tag_tree_zero(pband->zbp, precp->ncbw, precp->ncbh, 0);
          // codeblock
          // pband->blk = (blk_ *)malloc(sizeof(blk_) * precp->ncbw * precp->ncbh);
          pband->blk = (blk_ *)stackAlloc(sizeof(blk_) * precp->ncbw * precp->ncbh, 0);
#ifdef DEBUG
          count_allocations(sizeof(blk_) * precp->ncbw * precp->ncbh);
#endif
          for (uint32_t cb = 0; cb < precp->ncbw * precp->ncbh; cb++) {
            blk_ *blkp = &(pband->blk[cb]);
            // blkp->parent_band  = &(rp->band[b]);
            // blkp->parent_prec  = precp;
            const uint32_t x = cb & precp->ncbw;
            const uint32_t y = cb / precp->ncbw;
            blkp->coord.x0   = LOCAL_MAX(pbx0, cbw * (x + pbx0 / cbw));
            blkp->coord.y0   = LOCAL_MAX(pby0, cbh * (y + pby0 / cbh));
            blkp->coord.x1   = LOCAL_MIN(pbx1, cbw * (x + 1 + pbx1 / cbw));
            blkp->coord.y1   = LOCAL_MIN(pby1, cbh * (y + 1 + pby1 / cbh));
            blkp->incl       = 0;
            blkp->npasses    = 0;
            blkp->length     = 0;
            blkp->lblock     = 3;
            // blkp->nb_lengthinc = blkp->nb_terminations = blkp->nb_terminationsinc = 0;
            blkp->pass_lengths[0] = blkp->pass_lengths[1] = 0;
            // const uint32_t offset = blkp->coord.x0 - rp->band[b].coord.x0 +
            // (blkp->coord.y0 - rp->band[b].coord.y0) * stride;
          }
        }
      }
    }
  }
}

[[maybe_unused]] static inline int needs_termination(int style, int passno) {
  if (style & CMODE_BYPASS) {
    int type = passno % 3;
    passno /= 3;
    if (type == 0 && passno > 2) return 2;
    if (type == 2 && passno > 2) return 1;
    if (style & CMODE_TERMALL) {
      return passno > 2 ? 2 : 1;
    }
  }
  if (style & CMODE_TERMALL) return 1;
  return 0;
}

static int getlblockinc(codestream *s) {
  int res = 0, ret;
  while ((ret = s->packetheader_get_bits(1))) {
    if (ret < 0) return ret;
    res++;
  }
  return res;
}

static int parse_packet_header(codestream *s, prec_ *prec, const coc_marker *coc) {
  for (uint32_t b = 0; b < prec->num_bands; ++b) {
    uint32_t nb_code_blocks = prec->ncbw * prec->ncbh;
    pband_ *pband           = &(prec->pband[b]);
    for (uint32_t cblkno = 0; cblkno < nb_code_blocks; cblkno++) {
      blk_ *cblk = pband->blk + cblkno;
      int incl, newpasses, llen;

      if (!cblk->incl) {
        incl        = 0;
        cblk->modes = coc->cbs;
        if (cblk->modes >= CMODE_HT) cblk->ht_plhd = HT_PLHD_ON;
        // if (layno > 0) incl = tag_tree_decode(s, prec->cblkincl + cblkno, 0 + 1) == 0;
        incl = tag_tree_decode(s, pband->incl + cblkno, 0 + 1) == 0;

        if (incl) {
          int zbp = tag_tree_decode(s, pband->zbp + cblkno, 100);
          // int v   = expn[bandno] + numgbits - 1 - (zbp - tile->comp->roi_shift);
          // if (v < 0 || v > 30) {
          //   av_log(s->avctx, AV_LOG_ERROR, "nonzerobits %d invalid or unsupported\n", v);
          //   return AVERROR_INVALIDDATA;
          // }
          cblk->incl = 1;
          // cblk->nonzerobits = v;
          cblk->zbp    = zbp;
          cblk->lblock = 3;
        }
      } else {
        incl = s->packetheader_get_bits(1);
      }

      if (incl) {
        uint8_t bypass_term_threshold = 0;
        uint8_t bits_to_read          = 0;
        uint32_t segment_bytes        = 0;
        int32_t segment_passes        = 0;
        uint8_t next_segment_passes   = 0;
        // uint32_t tmp_length = 0;
        // int32_t newpasses_copy, npasses_copy;
        // Decode number of passes.
        newpasses = 1;
        newpasses += s->packetheader_get_bits(1);
        if (newpasses >= 2) {
          newpasses += s->packetheader_get_bits(1);
          if (newpasses >= 3) {
            newpasses += s->packetheader_get_bits(2);
            if (newpasses >= 6) {
              newpasses += s->packetheader_get_bits(5);
              if (newpasses >= 37) newpasses += s->packetheader_get_bits(7);
            }
          }
        }

        // if ((newpasses = getnpasses(s)) <= 0) return newpasses;
        if (cblk->npasses + newpasses >= JPEG2000_MAX_PASSES) {
          printf("Too many passes\n");
          return EXIT_FAILURE;
        }
        if ((llen = getlblockinc(s)) < 0) return llen;
        if (cblk->lblock + llen + int_log2(newpasses) > 16) {
          printf("Block with length beyond 16 bits\n");
          return EXIT_FAILURE;
        }
        cblk->lblock += llen;

        // Count number of necessary terminations for non HT code block
        // newpasses_copy = newpasses;
        // npasses_copy   = cblk->npasses;
        // if (!(cblk->modes & CMODE_HT)) {
        //   do {
        //     int newpasses1 = 0;
        //
        //     while (newpasses1 < newpasses_copy) {
        //       newpasses1++;
        //       if (needs_termination(coc->cbs, npasses_copy + newpasses1 - 1)) {
        //         cblk->nb_terminationsinc++;
        //         break;
        //       }
        //     }
        //     npasses_copy += newpasses1;
        //     newpasses_copy -= newpasses1;
        //   } while (newpasses_copy);
        // }

        if (cblk->ht_plhd) {
          int32_t pass_bound;
          int32_t href_passes;
          href_passes    = (cblk->npasses + newpasses - 1) % 3;
          segment_passes = newpasses - href_passes;
          pass_bound     = 2;
          bits_to_read   = cblk->lblock;
          if (segment_passes < 1) {
            // No possible HT Cleanup pass here; may have placeholder passes
            // or an original J2K block bit-stream (in MIXED mode).
            segment_passes = newpasses;
            while (pass_bound <= segment_passes) {
              bits_to_read++;
              pass_bound += pass_bound;
            }
            segment_bytes = s->packetheader_get_bits(bits_to_read);
            if (segment_bytes) {
              if (cblk->modes & HT_MIXED) {
                cblk->ht_plhd = HT_PLHD_OFF;
                cblk->modes &= (uint8_t)(~(CMODE_HT));
              } else {
                printf("Length information for a HT-codeblock is invalid\n");
                return EXIT_FAILURE;
              }
            }
          } else {
            while (pass_bound <= segment_passes) {
              bits_to_read++;
              pass_bound += pass_bound;
            }
            segment_bytes = s->packetheader_get_bits(bits_to_read);
            if (segment_bytes) {
              // No more placeholder passes
              if (!(cblk->modes & HT_MIXED)) {
                // Must be the first HT Cleanup pass
                if (segment_bytes < 2) {
                  printf("Length information for a HT-codeblock is invalid\n");
                  return EXIT_FAILURE;
                }
                next_segment_passes = 2;
                cblk->ht_plhd       = HT_PLHD_OFF;
                // Write length information for HT CleanUp segment
                cblk->pass_lengths[0] = segment_bytes;
              } else if (cblk->lblock > 3 && segment_bytes > 1
                         && (segment_bytes >> (bits_to_read - 1)) == 0) {
                // Must be the first HT Cleanup pass, since length MSB is 0
                next_segment_passes = 2;
                cblk->ht_plhd       = HT_PLHD_OFF;
                // Write length information for HT CleanUp segment
                cblk->pass_lengths[0] = segment_bytes;
              } else {
                // Must have an original (non-HT) block coding pass
                cblk->modes &= (uint8_t)(~(CMODE_HT));
                cblk->ht_plhd  = HT_PLHD_OFF;
                segment_passes = newpasses;
                while (pass_bound <= segment_passes) {
                  bits_to_read++;
                  pass_bound += pass_bound;
                  segment_bytes <<= 1;
                  segment_bytes += s->packetheader_get_bits(1);
                }
              }
            } else {
              // Probably parsing placeholder passes, but we need to read an
              // extra length bit to verify this, since prior to the first
              // HT Cleanup pass, the number of length bits read for a
              // contributing code-block is dependent on the number of passes
              // being included, as if it were a non-HT code-block.
              segment_passes = newpasses;
              if (pass_bound <= segment_passes) {
                while (1) {
                  bits_to_read++;
                  pass_bound += pass_bound;
                  segment_bytes <<= 1;
                  segment_bytes += s->packetheader_get_bits(1);
                  if (pass_bound > segment_passes) break;
                }
                if (segment_bytes) {
                  if (cblk->modes & HT_MIXED) {
                    cblk->modes &= (uint8_t)(~(CMODE_HT));
                    cblk->ht_plhd = HT_PLHD_OFF;
                  } else {
                    printf("Length information for a HT-codeblock is invalid\n");
                    return EXIT_FAILURE;
                  }
                }
              }
            }
          }
        } else if (cblk->modes & CMODE_HT) {
          // Quality layer commences with a non-initial HT coding pass
          if (bits_to_read != 0) {
            printf("Length information for a HT-codeblock is invalid\n");
            return EXIT_FAILURE;
          }
          segment_passes = cblk->npasses % 3;
          if (segment_passes == 0) {
            // newpasses is a HT Cleanup pass; next segment has refinement passes
            segment_passes      = 1;
            next_segment_passes = 2;
            if (segment_bytes == 1) printf("Length information for a HT-codeblock is invalid\n");
          } else {
            // newpasses = 1 means npasses is HT SigProp; 2 means newpasses is
            // HT MagRef pass
            segment_passes      = newpasses > 1 ? 3 - segment_passes : 1;
            next_segment_passes = 1;
            bits_to_read        = int_log2(segment_passes);
          }
          bits_to_read  = (uint8_t)(bits_to_read + cblk->lblock);
          segment_bytes = s->packetheader_get_bits(bits_to_read);
          // Write length information for HT Refinment segment
          cblk->pass_lengths[1] += segment_bytes;
        } else if (!(cblk->modes & (CMODE_TERMALL | CMODE_BYPASS))) {
          // Common case for non-HT code-blocks; we have only one segment
          bits_to_read   = (uint8_t)cblk->lblock + int_log2((uint8_t)newpasses);
          segment_bytes  = s->packetheader_get_bits(bits_to_read);
          segment_passes = newpasses;
        } else if (cblk->modes & CMODE_TERMALL) {
          // RESTART MODE
          bits_to_read        = cblk->lblock;
          segment_bytes       = s->packetheader_get_bits(bits_to_read);
          segment_passes      = 1;
          next_segment_passes = 1;
        } else {
          // BYPASS MODE
          bypass_term_threshold = 10;
          if (bits_to_read != 0) {
            printf("Length information for a codeblock in BYPASS is invalid\n");
            return EXIT_FAILURE;
          }
          if (cblk->npasses < bypass_term_threshold) {
            // May have from 1 to 10 uninterrupted passes before 1st RAW SigProp
            segment_passes = bypass_term_threshold - cblk->npasses;
            if (segment_passes > newpasses) segment_passes = newpasses;
            while ((2 << bits_to_read) <= segment_passes) bits_to_read++;
            next_segment_passes = 2;
          } else if ((cblk->npasses - bypass_term_threshold) % 3 < 2) {
            // 0 means newpasses is a RAW SigProp; 1 means newpasses is a RAW MagRef pass
            segment_passes      = newpasses > 1 ? 2 - (cblk->npasses - bypass_term_threshold) % 3 : 1;
            bits_to_read        = int_log2(segment_passes);
            next_segment_passes = 1;
          } else {
            // newpasses is an isolated Cleanup pass that precedes a RAW SigProp pass
            segment_passes      = 1;
            next_segment_passes = 2;
          }
          bits_to_read  = (uint8_t)(bits_to_read + cblk->lblock);
          segment_bytes = s->packetheader_get_bits(bits_to_read);
        }
        // Update cblk->npasses and write length information
        cblk->npasses = (uint8_t)(cblk->npasses + segment_passes);
        // cblk->lengthinc[cblk->nb_lengthinc++] = segment_bytes;

        if ((cblk->modes & CMODE_HT) && cblk->ht_plhd == HT_PLHD_OFF) {
          newpasses -= (uint8_t)segment_passes;
          while (newpasses > 0) {
            segment_passes      = newpasses > 1 ? next_segment_passes : 1;
            next_segment_passes = (uint8_t)(3 - next_segment_passes);
            bits_to_read        = (uint8_t)(cblk->lblock + int_log2(segment_passes));
            segment_bytes       = s->packetheader_get_bits(bits_to_read);
            newpasses -= (uint8_t)(segment_passes);
            // This is a FAST Refinement pass
            // Write length information for HT Refinement segment
            cblk->pass_lengths[1] += segment_bytes;
            // Update cblk->npasses and write length information
            cblk->npasses = (uint8_t)(cblk->npasses + segment_passes);
            // cblk->lengthinc[cblk->nb_lengthinc++] = segment_bytes;
          }
        } else {
          newpasses -= (uint8_t)(segment_passes);
          while (newpasses > 0) {
            if (bypass_term_threshold != 0) {
              segment_passes      = newpasses > 1 ? next_segment_passes : 1;
              next_segment_passes = (uint8_t)(3 - next_segment_passes);
              bits_to_read        = (uint8_t)(cblk->lblock + int_log2(segment_passes));
            } else {
              if ((cblk->modes & CMODE_TERMALL) == 0) {
                printf("Corrupted packet header is found.\n");
                return EXIT_FAILURE;
              }
              segment_passes = 1;
              bits_to_read   = cblk->lblock;
            }
            segment_bytes = s->packetheader_get_bits(bits_to_read);
            newpasses -= (uint8_t)(segment_passes);

            // Update cblk->npasses and write length information
            cblk->npasses = (uint8_t)(cblk->npasses + segment_passes);
            // cblk->lengthinc[cblk->nb_lengthinc++] = segment_bytes;
          }
        }
      } else {
        // This codeblock has no contribution to the current packet
        continue;
      }
      cblk->length = cblk->pass_lengths[0] + cblk->pass_lengths[1];
    }
  }
  s->packetheader_flush_bits();

  // read code-block data
  for (uint32_t b = 0; b < prec->num_bands; ++b) {
    uint32_t nb_code_blocks = prec->ncbw * prec->ncbh;
    pband_ *pband           = &prec->pband[b];
    for (uint32_t cblkno = 0; cblkno < nb_code_blocks; cblkno++) {
      blk_ *cblk = pband->blk + cblkno;
      cblk->data = (uint8_t *)s->get_address();
      if (cblk->length) {
        // recover Scup and modDcup()
        cblk->Scup =
            ((cblk->data[cblk->pass_lengths[0] - 1] << 4) + (cblk->data[cblk->pass_lengths[0] - 2] & 0x0F));
        // cblk->data[cblk->pass_lengths[0] - 1] = 0xFFu;
        // cblk->data[cblk->pass_lengths[0] - 2] |= 0x0Fu;
      }
      [[maybe_unused]] uint8_t bnum;
      if (prec->res_num == 0) {
        bnum = 0;
      } else {
        bnum = b + 1;
      }
      // printf("%d,%d,%d\n", prec->res_num, bnum, cblk->length);
      if (get_log_file_fp() != NULL) {
        fprintf(log_file, "%d,%d,%d\n", prec->res_num, bnum, cblk->length);
      }
      s->move_forward(cblk->length);
    }
  }
  return EXIT_SUCCESS;
}

int read_packet(codestream *buf, prec_ *prec, const coc_marker *coc) {
  [[maybe_unused]] uint16_t Lsop, Nsop;
  int ret;

  if (prec->use_SOP) {
    uint16_t word = buf->get_word();
    if (word != SOP) {
      printf("ERROR: Expected SOP marker but %04X is found\n", word);
      return EXIT_FAILURE;
    }
    Lsop = buf->get_word();
    if (Lsop != 4) {
      printf("ERROR: illegal Lsop value %d is found\n", Lsop);
      return EXIT_FAILURE;
    }
    Nsop = buf->get_word();
  }
  int bit = buf->packetheader_get_bits(1);

  if (bit == 0) {
    // if 0, empty packet
    buf->packetheader_flush_bits();  // flushing remaining bits of packet header
    if (prec->use_EPH) {
      uint16_t word = buf->get_word();
      if (word != EPH) {
        printf("ERROR: Expected EPH marker but %04X is found\n", word);
        return EXIT_FAILURE;
      }
    }
    return EXIT_SUCCESS;
  }
  ret = parse_packet_header(buf, prec, coc);
  return ret;
}

int read_tile(tile_ *tile, const coc_marker *cocs, const dfs_marker *dfs) {
  int ret;
  uint32_t PO, RS, RE;
  [[maybe_unused]] uint32_t LYE, CS, CE;
  int32_t step_x, step_y;

  PO  = cocs[0].progression_order;  // tile->progression_order;
  CS  = 0;                          // assume No POC marker
  RS  = 0;                          // assume No POC marker
  LYE = 1;                          // assume No Layer
  CE  = 3;                          // tile->num_components;

  uint32_t px[MAX_NUM_COMPONENTS][MAX_DWT_LEVEL + 1] = {0};
  uint32_t py[MAX_NUM_COMPONENTS][MAX_DWT_LEVEL + 1] = {0};

  // bool is_packet_read[3][6][4 * 270] = {false};
  switch (PO) {
    case PCRL:
      if (tile->crp.empty()) {
        step_x = 32;
        step_y = 32;
        for (uint32_t c = CS; c < CE; ++c) {
          const coc_marker *coc = &cocs[c];
          for (uint32_t r = RS; r < coc->NL + 1; ++r) {
            step_x = LOCAL_MIN(step_x, coc->prw[r]);
            step_y = LOCAL_MIN(step_y, coc->prh[r]);
          }
        }

        step_x = 1 << step_x;
        step_y = 1 << step_y;

        for (uint32_t y = tile->coord.y0; y < tile->coord.y1; y += step_y) {
          for (uint32_t x = tile->coord.x0; x < tile->coord.x1; x += step_x) {
            for (uint32_t c = CS; c < CE; ++c) {
              const coc_marker *coc = &cocs[c];
              tcomp_ *tcp           = &(tile->tcomp[c]);
              RE                    = coc->NL + 1;
              for (uint32_t r = RS; r < RE; ++r) {
                res_ *rp     = &tcp->res[r];
                uint32_t xNL = dfs->hor_depth[coc->NL - r];  // get_hor_depth(coc->NL - r, dfs);
                uint32_t yNL = dfs->ver_depth[coc->NL - r];  // get_ver_depth(coc->NL - r, dfs);

                if (!((x % (tcp->sub_x * (1U << (coc->prw[r] + xNL))) == 0)
                      || ((x == tile->coord.x0)
                          && ((rp->coord.x0 * (1U << (xNL))) % (1U << (coc->prw[r] + xNL)) != 0)))) {
                  continue;
                }
                if (!((y % (tcp->sub_y * (1U << (coc->prh[r] + yNL))) == 0)
                      || ((y == tile->coord.y0)
                          && ((rp->coord.y0 * (1U << (yNL))) % (1U << (coc->prh[r] + yNL)) != 0)))) {
                  continue;
                }

                uint32_t p = py[c][r] * rp->npw + px[c][r];
                // save identified precinct information as a sequence
                crp_status tmp{(uint8_t)c, (uint8_t)r, (uint16_t)p};
                tile->crp.push_back(tmp);

                prec_ *pp = &rp->prec[p];
                ret       = read_packet(tile->buf, pp, coc);
                if (ret) {
                  return ret;
                }

                px[c][r] += 1;
                if (px[c][r] == rp->npw) {
                  px[c][r] = 0;
                  py[c][r] += 1;
                }
              }
            }
          }
        }
      } else {
        // use saved sequence of precincts
        for (uint32_t i = 0; i < tile->crp.size(); ++i) {
          prec_ *pp = &tile->tcomp[tile->crp[i].c].res[tile->crp[i].r].prec[tile->crp[i].p];
          ret       = read_packet(tile->buf, pp, &cocs[tile->crp[i].c]);
          if (ret) {
            return ret;
          }
        }
      }
      break;

    case PRCL:
      break;
    default:
      printf("Only PCRL or PRCL are supported %d\n", PO);
      break;
  }
  return EXIT_SUCCESS;
}
