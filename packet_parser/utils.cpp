#include "utils.hpp"

#include <cassert>
#include <cstdlib>

#include "type.hpp"

FILE *log_file = nullptr;
FILE *get_log_file_fp() { return log_file; }
void log_init([[maybe_unused]] const size_t num_frame) {
#ifdef ENABLE_LOGGING
  char buf[128];
  snprintf(buf, 128, "log_%05lu.log", num_frame);
  log_file = fopen(buf, "w");
#endif
}
void log_put([[maybe_unused]] const char *msg) {
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

void save_j2c([[maybe_unused]] const size_t num_frame, [[maybe_unused]] const uint8_t *incoming_data,
              [[maybe_unused]] const size_t incoming_data_len) {
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

// Per-frame-resettable arena. The malloc fallback was removed because nothing in this
// parser frees the per-stream tile/precinct/codeblock structures it allocates — using
// malloc would leak everything at program exit. The arena is sized for one frame's
// worth of structure on the user's 4K@60 stream and must not overflow; if a future
// stream requires more, increase BUFSIZE.
void *stackAlloc(const size_t n, int reset) {
  g_allocated_bytes += n;
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
}