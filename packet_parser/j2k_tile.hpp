//
// Created by OSAMU WATANABE on 2024/11/21.
//

#ifndef J2K_TILE_H
#define J2K_TILE_H

#include "type.hpp"

tile_ *create_tiles(siz_marker *siz, coc_marker *cocs, dfs_marker *dfs, codestream *buf);
int read_tiles(tile_ *tiles, const siz_marker *siz, const coc_marker *cocs,  const dfs_marker *dfs);
void restart_tiles(tile_ *tiles, const siz_marker *siz);
void destroy_tiles(tile_ *tile, const siz_marker *siz);
#endif  // J2K_TILE_H
