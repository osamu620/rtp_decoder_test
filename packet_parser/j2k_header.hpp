//
// Created by OSAMU WATANABE on 2024/11/21.
//

#ifndef J2K_HEADER_H
#define J2K_HEADER_H

#include "type.hpp"

uint32_t parse_main_header(codestream *buf, siz_marker *siz, cod_marker *cod, coc_marker *cocs, qcd_marker *qcd,
                      dfs_marker *dfs);

// Returns Ccap[15] from the most recently parsed main header (0 if no CAP marker).
uint16_t get_Ccap15();

#endif  // J2K_HEADER_H_H
