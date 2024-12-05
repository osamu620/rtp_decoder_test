#pragma once

#include <cstdio>
#include <cstddef>
#include "type.hpp"

#define AV_LOG_WARNING 0

#define int_log2(x) (31 - __builtin_clz((x) | 1))

FILE *get_log_file_fp();
void log_init(const char *file_name);
void log_close();

void *stackAlloc(size_t n, int reset);