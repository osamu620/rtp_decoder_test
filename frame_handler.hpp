//
// Created by OSAMU WATANABE on 2024/12/01.
//

#ifndef FRAME_HANDLER_HPP
#define FRAME_HANDLER_HPP
#include <cstring>
#include <chrono>

namespace j2k {
extern "C" {
#include <packet_parser/j2k_header.h>
#include <packet_parser/j2k_tile.h>
#include <packet_parser/utils.h>
}

class frame_handler {
 private:
  uint8_t* incoming_data;
  size_t incoming_data_len;
  size_t total_frames;
  size_t trunc_frames;
  size_t lost_frames;
  uint32_t start_SOD;
  siz_marker siz;
  cod_marker cod;
  coc_marker cocs[MAX_NUM_COMPONENTS];
  qcd_marker qcd;
  dfs_marker dfs;
  codestream cs;
  tile_* tiles;
  std::chrono::time_point<std::chrono::high_resolution_clock> start_time;

 public:
  frame_handler(uint8_t* p)
      : incoming_data(p),
        incoming_data_len(0),
        total_frames(0),
        trunc_frames(0),
        lost_frames(0),
        start_SOD(0),
        siz({}),
        cod({}),
        qcd({}),
        dfs({}),
        cs({}),
        tiles(nullptr) {
    cs.src = &incoming_data[0];
    start_time = std::chrono::high_resolution_clock::now();
  }

  size_t get_total_frames() const { return total_frames; }

  double get_duration() {
    auto duration = std::chrono::high_resolution_clock::now() - start_time;
    auto count    = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    start_time    = std::chrono::high_resolution_clock::now();
    return static_cast<double>(count) / 1000;
  }

  inline void countup_lost_frames() { this->lost_frames++; }
  inline size_t get_lost_frames() const { return this->lost_frames; }
  inline size_t get_trunc_frames() const { return this->trunc_frames; }

  ~frame_handler() {
    restart_tiles(tiles, &siz);
    // destroy_tiles(tiles, &siz);
    incoming_data_len = 0;
  }

  void restart() {
    restart_tiles(tiles, &siz);
    // destroy_tiles(tiles, &siz);
    // tiles             = nullptr;
    incoming_data_len = 0;
    cs.pos            = 0;
    cs.bits           = 0;
    cs.last           = 0;
    cs.tmp            = 0;
  }

  void pull_data(uint8_t* data, size_t size, int MH, int marker) {
    std::memcpy(this->incoming_data + incoming_data_len, data, size);
    incoming_data_len += size;
    if (MH >= 2) {
      if (tiles == nullptr) {
        start_SOD = parse_main_header(&cs, &siz, &cod, cocs, &qcd, &dfs);
        tiles = create_tiles(&siz, cocs, &dfs, &cs);
      } else {
        // skip main header parsing (re-use)
        tiles->buf->pos = start_SOD;
      }
    }
    if (marker) {
      char buf[128];
      snprintf(buf, 128, "out_%05lu.j2c", total_frames);
      FILE* fp = fopen(buf, "wb");
      fwrite(this->incoming_data, 1, incoming_data_len, fp);
      fclose(fp);

      snprintf(buf, 128, "log_%05lu.log", total_frames);
      log_init(buf);
      process();
      log_close();
      // printf("%d bytes allocated\n", get_bytes_allocated());
    }
  }

  void process() {
    if (tiles != nullptr) {
      if (read_tiles(tiles, &siz, cocs, &dfs)) {
        trunc_frames++;
      }
      total_frames++;
      restart();
    }
  }
};

}  // namespace j2k

#endif  // FRAME_HANDLER_HPP
