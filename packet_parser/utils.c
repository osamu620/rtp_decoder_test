#include "utils.h"

#include <assert.h>
#include <stdlib.h>

#include "type.h"

FILE *log_file = NULL;
void log_init(const char *file_name) { log_file = fopen(file_name, "w"); }
void log_close() {
  if (log_file != NULL) {
    fclose(log_file);
  }
}

uint32_t g_allocated_bytes = 0;
#ifdef DEBUG
void count_allocations(uint32_t n) { g_allocated_bytes += n; }
uint32_t get_bytes_allocated(void) { return g_allocated_bytes; }
#endif

#define BUFSIZE (1024 * 30000)
void *stackAlloc(const size_t n, int reset) {
  g_allocated_bytes += n;
  // return malloc(n);
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
}
uint32_t get_bytes_allocated(void) { return g_allocated_bytes; }

void av_log(uint8_t dummy0, void *dummy1, char *msg) { printf("%s\n", msg); }

uint8_t get_byte(codestream *buf) {
  uint8_t byte = buf->src[buf->pos];
  buf->tmp     = 0;
  buf->bits    = 0;
  buf->pos++;
  return byte;
}

uint16_t get_word(codestream *buf) {
  uint16_t word = get_byte(buf);
  word <<= 8;
  word |= get_byte(buf);
  return word;
}

uint32_t get_dword(codestream *buf) {
  uint32_t dword = get_word(buf);
  dword <<= 16;
  dword |= get_word(buf);
  return dword;
}

// uint32_t get_hor_depth(uint32_t lev, dfs_marker *dfs) {
//   uint32_t D = 0;
//   for (uint32_t l = 1; l <= lev; ++l) {
//     if (!dfs->h_orient[l]) {
//       ++D;
//     }
//   }
//   return D;
// }
//
// uint32_t get_ver_depth(uint32_t lev, dfs_marker *dfs) {
//   uint32_t D = 0;
//   for (uint32_t l = 1; l <= lev; ++l) {
//     if (!dfs->v_orient[l]) {
//       ++D;
//     }
//   }
//   return D;
// }