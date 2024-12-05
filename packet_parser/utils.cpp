#include "utils.hpp"

#include <cassert>
#include <cstdlib>

#include "type.hpp"

FILE *log_file = NULL;
FILE *get_log_file_fp() { return log_file; }
void log_init(const char *file_name) { log_file = fopen(file_name, "w"); }
void log_close() {
  if (log_file != NULL) {
    fclose(log_file);
    log_file = NULL;
  }
}

uint32_t g_allocated_bytes = 0;
#ifdef DEBUG
void count_allocations(uint32_t n) { g_allocated_bytes += n; }
uint32_t get_bytes_allocated(void) { return g_allocated_bytes; }
#endif

#define BUFSIZE (1024 * 3000)
void *stackAlloc(const size_t n, int reset) {
  g_allocated_bytes += n;
#ifndef STACK_ALLOC
  return malloc(n);
#else
  static char buffer[BUFSIZE] = {0};
  static size_t index         = 0;
  if (reset) {
    index = 0;
    return NULL;
  }
  if (index + n > BUFSIZE) {
    printf("Out of memory\n");
    return NULL;
  }
  char *out = buffer + index;
  index += n;
  return (void *)out;
#endif
}