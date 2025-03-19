#include "utils.hpp"

#include <cassert>
#include <cstdlib>

#include "type.hpp"

FILE *log_file = nullptr;
FILE *get_log_file_fp() { return log_file; }
void log_init(const size_t num_frame) {
#ifdef ENABLE_LOGGING
  char buf[128];
  snprintf(buf, 128, "log_%05lu.log", num_frame);
  log_file = fopen(buf, "w");
#endif
}
void log_put(const char *msg) {
#ifdef ENABLE_LOGGING
  fprintf(log_file, "%s\n", msg);
#endif
}

void log_close() {
#ifdef ENABLE_LOGGING
  if (log_file != nullptr) {
    fclose(log_file);
    log_file = nullptr;
  }
#endif
}

void save_j2c(const size_t num_frame, const uint8_t *incoming_data, const size_t incoming_data_len) {
#ifdef ENABLE_SAVEJ2C
  char buf[128];
  snprintf(buf, 128, "out_%05lu.j2c", num_frame);
  FILE *fp = fopen(buf, "wb");
  fwrite(incoming_data, 1, incoming_data_len, fp);
  fclose(fp);
#endif
}

uint32_t g_allocated_bytes = 0;
#ifdef DEBUG
void count_allocations(uint32_t n) { g_allocated_bytes += n; }
uint32_t get_bytes_allocated(void) { return g_allocated_bytes; }
#endif

#define BUFSIZE (1024 * 4096)

void *stackAlloc(const size_t n, int reset) {
  g_allocated_bytes += n;
#ifndef STACK_ALLOC
  return malloc(n);
#else
  alignas(8) static uint8_t buffer[BUFSIZE] = {0};
  static size_t index                       = 0;
  if (reset) {
    index = 0;
    return NULL;
  }
  if (index + n > BUFSIZE) {
    printf("Out of memory\n");
    return NULL;
  }
  uint8_t *out = buffer + index;
  index += n;
  return (void *)out;
#endif
}