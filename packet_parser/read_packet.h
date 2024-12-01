#pragma once
#include <stdio.h>
#include <stdlib.h>

#include "utils.h"

int read_packet(codestream *buf, IO_map *iomap) {
  uint32_t PO = iomap->ResLevel_Porder & 1;
  uint32_t NL = (iomap->ResLevel_Porder >> 4) & 0x7;
  uint32_t LYE = 1;
  uint32_t RS = 0;
  uint32_t RE = NL + 1;
  uint32_t CS = 0;
  uint32_t CE = 3;  // 3 components only

  uint32_t *pxn = iomap->PPx;
  uint32_t *pyn = iomap->PPy;

  // find gcd for precinct size, pxd and pyd are gcd
  uint32_t pxd = 16, pyd = 16;
  for (uint32_t r = 0; r <= NL; ++r) {
    if (pxd > pxn[r]) {
      pxd = pxn[r];
    }
    if (pyd > pyn[r]) {
      pyd = pyn[r];
    }
  }

  switch (PO) {
    case 3:  // PCRL
      for (uint32_t y = 0;) break;

    case 5:  // PRCL
      break;
    default:
      printf("PCRL and PRCL are only supported\n");
      return EXIT_FAILURE;
      break;
  }
}