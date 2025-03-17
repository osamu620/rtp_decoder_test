//
// Created by OSAMU WATANABE on 2024/11/21.
//

#ifndef J2K_TILE_H
#define J2K_TILE_H

#include "type.hpp"

void restart_tiles(tile_ *tiles, const siz_marker *siz);
void destroy_tiles(tile_ *tile, const siz_marker *siz);
#endif  // J2K_TILE_H
