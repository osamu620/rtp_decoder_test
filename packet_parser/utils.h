#pragma once
#include "stdio.h"
#include "type.h"

#define AV_LOG_WARNING 0

#define int_log2(x) (31 - __builtin_clz((x) | 1))

void log_init(const char *file_name);
void log_close();

void *stackAlloc(size_t n, int reset);
uint8_t get_byte(codestream *buf);
uint16_t get_word(codestream *buf);
uint32_t get_dword(codestream *buf);

// uint32_t get_hor_depth(uint32_t lev, dfs_marker *dfs);
// uint32_t get_ver_depth(uint32_t lev, dfs_marker *dfs);

void av_log(uint8_t dummy0, void *dummy1, char *msg);