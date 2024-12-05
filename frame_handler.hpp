//
// Created by OSAMU WATANABE on 2024/12/01.
//

#ifndef FRAME_HANDLER_HPP
#define FRAME_HANDLER_HPP
#include <cstring>
#include <chrono>
#include <packet_parser/tile_handler.hpp>
#include <packet_parser/j2k_header.hpp>
#include <packet_parser/j2k_tile.hpp>
#include <packet_parser/utils.hpp>
#include <arm_neon.h>

namespace j2k {

class frame_handler {
 private:
  uint8_t* const incoming_data;  // buffer preserves incoming RTP frame data
  size_t incoming_data_len;      // length of `incoming_data`
  tile_hanlder TH;
  size_t total_frames;  // total number of frames processed
  size_t trunc_frames;  // total number of truncated frames
  size_t lost_frames;   // total number of lost RTP frames (not J2K frames)
  uint32_t start_SOD;   // position of where SOD marker locates
  codestream cs;        //
  std::chrono::time_point<std::chrono::high_resolution_clock> start_time;  // for FPS calculation

 public:
  frame_handler(uint8_t* p)
      : incoming_data(p),
        incoming_data_len(0),
        total_frames(0),
        trunc_frames(0),
        lost_frames(0),
        start_SOD(0),
        cs(incoming_data) {
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
// #ifdef STACK_ALLOC
    //     restart_tiles(tiles, &siz);
    // #else
    //     destroy_tiles(tiles, &siz);
    // #endif
    incoming_data_len = 0;
  }
  //
  //   /** @brief restart processing for a next J2K frame
  //    *  @details This function shall be called after finishing packet parsing for a J2K frame
  //    */
  //   void restart() {
  // #ifdef STACK_ALLOC
  //     restart_tiles(tiles, &siz);
  // #else
  //     destroy_tiles(tiles, &siz);
  //     tiles = nullptr;
  // #endif
  //     incoming_data_len = 0;
  //     cs.pos            = 0;
  //     cs.bits           = 0;
  //     cs.last           = 0;
  //     cs.tmp            = 0;
  //   }

  void pull_data(uint8_t* data, size_t size, int MH, int marker) {
    if (MH >= 2) {
      incoming_data_len = 0;
      std::memcpy(this->incoming_data + incoming_data_len, data, size);
      if (!TH.is_ready()) {
        start_SOD =
            parse_main_header(&cs, TH.get_siz(), TH.get_cod(), TH.get_cocs(), TH.get_qcd(), TH.get_dfs());
        TH.create(&cs);
        if (start_SOD == 0) {
          return;
        }
      } else {
        // skip main header parsing (re-use)
        TH.restart(start_SOD);
      }
    } else {
#ifdef __ARM_NEON
      for (int i = 0; i < size; i += 16) {
        auto src = vld1q_u8(data + i);
        vst1q_u8(this->incoming_data + incoming_data_len + i, src);
      }
      size_t s = size - size % 16;
      std::memcpy(this->incoming_data + incoming_data_len + s, data + s, size % 16);
#else
      std::memcpy(this->incoming_data + incoming_data_len, data, size);
#endif
    }
    incoming_data_len += size;
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
    if (TH.read()) {
      trunc_frames++;
    }
    total_frames++;
  }

  s
};
} // namespace j2k

#endif  // FRAME_HANDLER_HPP
