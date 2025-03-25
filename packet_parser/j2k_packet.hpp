//
// Created by OSAMU WATANABE on 2024/11/21.
//

#ifndef J2K_PACKET_H
#define J2K_PACKET_H

#include "type.hpp"

void tag_tree_zero(tagtree_node *t, uint32_t w, uint32_t h, uint32_t val);
int read_tile(tile_ *tile, const coc_marker *coc, const dfs_marker *dfs);
int parse_one_precinct(tile_ *tile, const coc_marker *cocs);

int prepare_precinct_structure(tile_ *tile, const coc_marker *coc, const dfs_marker *dfs);

void parepare_tcomp_structure(tcomp_ *tcp, coc_marker *coc, dfs_marker *dfs);

#endif  // J2K_PACKET_H
