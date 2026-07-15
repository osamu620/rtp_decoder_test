//
// Created by OSAMU WATANABE on 2024/11/21.
//

#ifndef J2K_HEADER_H
#define J2K_HEADER_H

#include "type.hpp"

uint32_t parse_main_header(codestream *buf, siz_marker *siz, cod_marker *cod, coc_marker *cocs,
                           qcd_marker *qcd, dfs_marker *dfs);

// Returns Ccap[15] from the most recently parsed main header (0 if no CAP marker).
uint16_t get_Ccap15();

// Geometry signature over a COMPLETE main header in one contiguous buffer (the raw
// J2K bytes of an MH>=2 packet): FNV-1a 64 over the geometry-defining marker segments
// SIZ/COD/COC/QCD/QCC/DFS/ATK (marker code + length + payload, in stream order).
// Rate-only encoder re-dials leave all of these untouched, so equal signatures mean
// the latched tile structure is still valid for the new frame; any geometry change
// (image size, sampling, decomposition, codeblock geometry, quantization, or the DWT
// kernel — an i5x3 <-> i9x7 flip lives in COD + ATK) changes the hash.
// On success *sod_off = the byte offset just past the SOD marker (== the tile body
// start, == what parse_main_header returns) and the return value is non-zero.
// Returns 0 if the buffer is not a well-formed complete main header (no SOC, lost
// framing, or runs out before SOD).
uint64_t geometry_signature(const uint8_t *mh, size_t len, uint32_t *sod_off);

#endif  // J2K_HEADER_H_H
