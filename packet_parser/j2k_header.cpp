//
// Created by OSAMU WATANABE on 2024/11/21.
//

#include <cstdio>
#include <cstdlib>

#include "j2k_header.hpp"
#include "utils.hpp"

static int parse_SIZ(codestream *buf, siz_marker *siz) {
  [[maybe_unused]] uint16_t len = buf->get_word();  // Lsiz

  siz->Rsiz = buf->get_word();

  siz->Xsiz = buf->get_dword();
  siz->Ysiz = buf->get_dword();

  siz->XOsiz = buf->get_dword();
  siz->YOsiz = buf->get_dword();

  siz->XTsiz = buf->get_dword();
  siz->YTsiz = buf->get_dword();

  siz->XTOsiz = buf->get_dword();
  siz->YTOsiz = buf->get_dword();

  siz->Csiz = buf->get_word();
  if (siz->Csiz != 3) {
    printf("Only supports Csiz == 3\n");
    return EXIT_FAILURE;
  }
  for (uint16_t c = 0; c < siz->Csiz; c++) {
    siz->Ssiz[c] = buf->get_byte();
    if (siz->Ssiz[c] != 11) {
      printf("Only supports 12 bit/pixel/component\n");
      // return EXIT_FAILURE;
    }
    siz->XRsiz[c] = buf->get_byte();  // XRsiz, 1 or 2
    siz->YRsiz[c] = buf->get_byte();  // YRsiz, 1 or 2
  }

  return EXIT_SUCCESS;
}

static int parse_COD(codestream *buf, siz_marker *siz, cod_marker *cod) {
  [[maybe_unused]] uint16_t len = buf->get_word();  // Lcod

  //   for (uint16_t i = 0; i < len - 2; ++i) {
  //     buf->get_byte();
  //   }

  uint16_t tmp;

  // Scod
  uint8_t v         = buf->get_byte();
  cod->max_precinct = !(v & 1);
  if (cod->max_precinct > 1) {
    printf("Illegal Scod parameter value in COD\n");
  }
  cod->use_SOP                = (v >> 1) & 1;
  cod->use_EPH                = (v >> 2) & 1;
  cod->use_h_offset           = (v >> 3) & 1;
  cod->use_v_offset           = (v >> 4) & 1;
  cod->use_blkcoder_extension = (v >> 5) & 1;

  // Progression order
  cod->progression_order = buf->get_byte();
  cod->num_layers        = buf->get_word();  // number of layers
  cod->mct               = buf->get_byte();  // color transform

  // SPcod
  cod->NL = buf->get_byte();
  if (cod->NL < 1 || cod->NL > 5) {
    printf("Supprted DWT level is from 1 to 5.\n");
  }

  // codeblock width
  cod->cbw = buf->get_byte() + 2;
  if (cod->cbw < 7 || cod->cbw > 9) {
    printf("codeblock width %d is not supported\n", 1 << cod->cbw);
  }

  // codeblock height
  cod->cbh = buf->get_byte() + 2;
  if (cod->cbh > 4) {
    printf("codeblock height %d is not supported\n", 1 << cod->cbh);
    return EXIT_FAILURE;
  }

  cod->cbs = buf->get_byte();  // codeblock style
  if ((cod->cbs & 0x8) == 0) {
    printf("VCAUSAL shall be used\n");
  }

  cod->transform = buf->get_byte();  // DWT type

  if (siz->Rsiz & 0x10) {   // SSO
    tmp = buf->get_word();  // get SSO overlap value
  }

  // cod->prh = (uint8_t *)calloc(cod->NL + 1, sizeof(uint8_t));
  // cod->prw = (uint8_t *)calloc(cod->NL + 1, sizeof(uint8_t));
  for (int i = 0; i <= cod->NL; ++i) {
    tmp         = buf->get_byte();
    cod->prw[i] = tmp & 0xF;
    cod->prh[i] = (tmp >> 4) & 0xF;
  }

  return EXIT_SUCCESS;
}

static void copy_cod(cod_marker *cod, coc_marker *coc) {
  coc->max_precinct           = cod->max_precinct;
  coc->use_SOP                = cod->use_SOP;
  coc->use_EPH                = cod->use_EPH;
  coc->use_h_offset           = cod->use_h_offset;
  coc->use_v_offset           = cod->use_v_offset;
  coc->use_blkcoder_extension = cod->use_blkcoder_extension;
  coc->progression_order      = cod->progression_order;
  coc->num_layers             = cod->num_layers;
  coc->mct                    = cod->mct;
  coc->NL                     = cod->NL;
  coc->cbw                    = cod->cbw;
  coc->cbh                    = cod->cbh;
  coc->transform              = cod->transform;
  coc->cbs                    = cod->cbs;
  coc->SSO                    = cod->SSO;
  // coc->prh = (uint8_t *)calloc(cod->NL + 1, sizeof(uint8_t));
  // coc->prw = (uint8_t *)calloc(cod->NL + 1, sizeof(uint8_t));
  for (int i = 0; i <= cod->NL; ++i) {
    coc->prw[i] = cod->prw[i];
    coc->prh[i] = cod->prh[i];
  }
}

static int parse_COC(codestream *buf, siz_marker *siz, coc_marker *cocs) {
  uint16_t len = buf->get_word() - 2;  // Lcoc
  uint16_t tmp;

  // Ccoc
  coc_marker *coc;
  uint8_t v = buf->get_byte();
  len--;
  coc      = &cocs[v];
  coc->idx = v;

  // Scoc
  v = buf->get_byte();
  len--;
  coc->max_precinct = !(v & 1);
  if (coc->max_precinct > 1) {
    printf("Illegal Scod parameter value in COD\n");
  }
  coc->use_SOP                = (v >> 1) & 1;
  coc->use_EPH                = (v >> 2) & 1;
  coc->use_h_offset           = (v >> 3) & 1;
  coc->use_v_offset           = (v >> 4) & 1;
  coc->use_blkcoder_extension = (v >> 5) & 1;

  // SPcoc
  coc->NL = buf->get_byte();
  len--;
  // if (coc->NL < 1 || coc->NL > 5) {
  //   printf("Supprted DWT level is from 1 to 5.\n");
  // }

  // codeblock width
  coc->cbw = buf->get_byte() + 2;
  len--;
  if (coc->cbw < 7 || coc->cbw > 9) {
    printf("codeblock width %d is not supported\n", 1 << coc->cbw);
  }

  // codeblock height
  coc->cbh = buf->get_byte() + 2;
  len--;
  if (coc->cbh > 4) {
    printf("codeblock height %d is not supported\n", 1 << coc->cbh);
    return EXIT_FAILURE;
  }

  coc->cbs = buf->get_byte();  // codeblock style
  len--;
  if ((coc->cbs & 0x8) == 0) {
    printf("VCAUSAL shall be used\n");
  }

  coc->transform = buf->get_byte();  // DWT type
  len--;
  if (siz->Rsiz & 0x10) {   // SSO
    tmp = buf->get_word();  // get SSO overlap value
  }

  coc->step_x = 16;
  coc->step_y = 16;
  for (int i = 0; i < len; ++i) {
    tmp         = buf->get_byte();
    coc->prw[i] = tmp & 0xF;
    coc->prh[i] = (tmp >> 4) & 0xF;
    coc->step_x = (coc->prw[i] < coc->step_x) ? coc->prw[i] : coc->step_x;
    coc->step_y = (coc->prh[i] < coc->step_y) ? coc->prh[i] : coc->step_y;
  }

  return EXIT_SUCCESS;
}

static int parse_DFS(codestream *buf, dfs_marker *dfs) {
  uint16_t len = buf->get_word();  // Ldfs
  if (len < 5) {
    printf("Invalid DFS\n");
    return EXIT_FAILURE;
  }
  dfs->idx = buf->get_word();  // Sdfs: See Table A.9 in Part 2

  dfs->Idfs = buf->get_byte();  // Idfs: Number of elements in the string defining
                                // the number of decomposition sub-levels.

  uint64_t bits = 0;
  for (uint16_t i = 0; i < len - 5; ++i) {
    bits <<= 8;
    bits |= buf->get_byte();
  }
  bits >>= ceildiv_int(2 * dfs->Idfs, 8) * 8 - 2 * dfs->Idfs;  // discard padding bits

  uint32_t ho[4] = {0, 0, 0, 1};
  uint32_t vo[4] = {0, 0, 1, 0};
  for (uint8_t i = dfs->Idfs; i > 0; --i) {
    dfs->type[i]     = bits & 0x3;  // i start from 1 because we skip lowest LL or LX
    dfs->h_orient[i] = ho[dfs->type[i]];
    dfs->v_orient[i] = vo[dfs->type[i]];
    bits >>= 2;
  }
  // GET_HOR_DEPTH, Figure F.3 in ISO/IEC 15444-2
  for (uint32_t lev = 0; lev <= dfs->Idfs; ++lev) {
    uint32_t D = 0;
    for (uint32_t l = 1; l <= lev; ++l) {
      if (!dfs->h_orient[l]) {
        ++D;
      }
    }
    dfs->hor_depth[lev] = D;
  }
  // GET_VER_DEPTH, Figure F.3 in ISO/IEC 15444-2
  for (uint32_t lev = 0; lev <= dfs->Idfs; ++lev) {
    uint32_t D = 0;
    for (uint32_t l = 1; l <= lev; ++l) {
      if (!dfs->v_orient[l]) {
        ++D;
      }
    }
    dfs->ver_depth[lev] = D;
  }

  return EXIT_SUCCESS;
}

static int parse_QCD(codestream *buf, cod_marker *cod, qcd_marker *qcd, dfs_marker *dfs) {
  [[maybe_unused]] uint16_t len = buf->get_word();  // Lqcd
  uint8_t Sqcd                  = buf->get_byte();  // Sqcd
  qcd->type                     = Sqcd & 0x1F;
  qcd->num_guard_bits           = (Sqcd >> 5) & 0x7;

  uint8_t NL = (qcd->type == 1) ? 1 : cod->NL;

  // uint8_t num_bands = 1;
  // for (uint8_t i = 1; i <= NL; ++i) {
  //   num_bands += dfs->type[i] == BOTH ? 3 : 1;
  // }
  if (qcd->type == 0) {
    qcd->exponent[0] = buf->get_byte() >> 3;
    for (uint8_t i = 1; i <= NL; ++i) {
      if (dfs->type[i] == BOTH) {
        for (uint8_t b = 0; b < 3; ++b) {
          qcd->exponent[3 * (i - 1) + b + 1] = buf->get_byte() >> 3;
        }
      } else {
        qcd->exponent[3 * (i - 1) + 1] = buf->get_byte() >> 3;
      }
    }
  } else {
    uint16_t tmp     = buf->get_word();
    qcd->exponent[0] = tmp >> 11;
    qcd->mantissa[0] = tmp & 0x7FF;
    for (uint8_t i = 1; i <= NL; ++i) {
      if (dfs->type[i] == BOTH) {
        for (uint8_t b = 0; b < 3; ++b) {
          tmp                                = buf->get_word();
          qcd->exponent[3 * (i - 1) + b + 1] = tmp >> 11;
          qcd->mantissa[3 * (i - 1) + b + 1] = tmp & 0x7FF;
        }
      } else {
        tmp                            = buf->get_word();
        qcd->exponent[3 * (i - 1) + 1] = tmp >> 11;
        qcd->mantissa[3 * (i - 1) + 1] = tmp & 0x7FF;
      }
    }
  }
  return EXIT_SUCCESS;
}

static int skip_marker(codestream *buf) {
  uint16_t len = buf->get_word();  // Lmar
  for (uint16_t i = 0; i < len - 2; ++i) {
    buf->get_byte();
  }
  return EXIT_SUCCESS;
}

void print_SIZ(siz_marker *siz, coc_marker *cocs, dfs_marker *dfs) {
  printf("Xsiz = %d\n", siz->Xsiz);
  printf("Ysiz = %d\n", siz->Ysiz);
  printf("XOsiz = %d\n", siz->XOsiz);
  printf("YOsiz = %d\n", siz->YOsiz);
  printf("XTsiz = %d\n", siz->XTsiz);
  printf("YTsiz = %d\n", siz->YTsiz);
  printf("XTOsiz = %d\n", siz->XTOsiz);
  printf("YTOsiz = %d\n", siz->YTOsiz);

  for (uint16_t c = 0; c < siz->Csiz; ++c) {
    coc_marker *coc = &cocs[c];
    printf("codeblock width  = %d\n", 1 << coc->cbw);
    printf("codeblock height = %d\n", 1 << coc->cbh);

    uint32_t NL = coc->NL;
    if (NL > 32) {
      NL = dfs->Idfs;
    }
    printf("DWT level = %d\n", NL);
    printf("Progression order = %d\n", coc->progression_order);

    for (uint32_t r = 0; r <= NL; ++r) {
      printf("Precinct size @ LV %d = %dx%d\n", r, 1 << coc->prw[r], 1 << coc->prh[r]);
    }
  }
}

uint32_t parse_main_header(codestream *buf, siz_marker *siz, cod_marker *cod, coc_marker *cocs,
                           qcd_marker *qcd, dfs_marker *dfs) {
  if (buf->get_word() != SOC) {
    printf("invalid j2c\n");
    return 0;
  }

  uint16_t marker;
  while ((marker = buf->get_word()) != SOD) {
    switch (marker) {
      case SIZ:
        parse_SIZ(buf, siz);
        break;
      case COD:
        parse_COD(buf, siz, cod);
        copy_cod(cod, &cocs[0]);
        copy_cod(cod, &cocs[1]);
        copy_cod(cod, &cocs[2]);
        break;
      case QCD:
        parse_QCD(buf, cod, qcd, dfs);
        break;
      case COC:
        parse_COC(buf, siz, cocs);
        break;
      case DFS:
        parse_DFS(buf, dfs);
        break;
      default:
        // printf("Skipped %4x\n", marker);
        skip_marker(buf);
        break;
    }
  }
  return buf->get_pos();
}
