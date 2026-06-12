#pragma once

#include <cstdio>
#include <cstddef>
#include "type.hpp"

#define AV_LOG_WARNING 0

#define int_log2(x) (31 - __builtin_clz((x) | 1))

// ENABLE_SAVEJ2C / ENABLE_LOGGING are CMake options:
//   cmake -B build -DENABLE_LOGGING=ON -DENABLE_SAVEJ2C=ON

FILE *get_log_file_fp();
void log_init(const size_t num_frame);
void log_put(const char *msg);
void log_close();
void save_j2c(const size_t num_frame, const uint8_t *incoming_data, const size_t incoming_data_len);
void *stackAlloc(size_t n, int reset);
