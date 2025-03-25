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

#define ACTION(func, ...)                                                         \
  auto st = std::chrono::high_resolution_clock::now();                            \
  if (tile_hndr.func(__VA_ARGS__)) {                                              \
    log_put("**************** FAILURE");                                          \
    is_parsing_failure = 1;                                                       \
  }                                                                               \
  auto dr    = std::chrono::high_resolution_clock::now() - st;                    \
  auto count = std::chrono::duration_cast<std::chrono::microseconds>(dr).count(); \
  cumlative_time += static_cast<double>(count)

namespace j2k {
class frame_handler {
 private:
  uint8_t *const incoming_data;  // buffer preserves incoming RTP frame data
  size_t incoming_data_len;      // length of `incoming_data`
  size_t total_frames;           // total number of frames processed
  size_t trunc_frames;           // total number of truncated frames
  size_t lost_frames;            // total number of lost RTP frames (not J2K frames)
  uint32_t start_SOD;            // position of where SOD marker locates
  int32_t is_parsing_failure;    //
  std::chrono::time_point<std::chrono::high_resolution_clock> start_time;  // for FPS calculation
  double cumlative_time;
  tile_hanlder tile_hndr;
  codestream cs;

 public:
  frame_handler(uint8_t *p)
      : incoming_data(p),
        incoming_data_len(0),
        total_frames(0),
        trunc_frames(0),
        lost_frames(0),
        start_SOD(0),
        is_parsing_failure(0),
        cumlative_time(0.0),
        cs(incoming_data) {
    start_time = std::chrono::high_resolution_clock::now();
  }

  size_t get_total_frames() const { return total_frames; }

  double get_cumlative_time_then_reset() {
    double ret     = cumlative_time;
    cumlative_time = 0.0;
    return ret;
  }

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

  void pull_data(uint8_t *data, size_t size, int MH, int marker, uint32_t POS_PID) {
    const uint32_t POS = POS_PID >> 20;
    const uint32_t PID = POS_PID & 0x000FFFFF;

    // std::memcpy(this->incoming_data + incoming_data_len, data, size);
    std::memcpy(reinterpret_cast<uint32_t *>(this->incoming_data) + (incoming_data_len >> 2), data, size);
    if (MH >= 1) {  // MH >=1 means this RTP packet is Main packet.
      log_init(total_frames);
      incoming_data_len = 0;
      if (!tile_hndr.is_ready()) {
        if (MH >= 2) {  // MH >= 2 means all the Main packet has been received.
          start_SOD = parse_main_header(&cs, tile_hndr.get_siz(), tile_hndr.get_cod(), tile_hndr.get_cocs(),
                                        tile_hndr.get_qcd(), tile_hndr.get_dfs());
          tile_hndr.create(&cs);
        } else {
          start_SOD += size;
        }
        if (start_SOD == 0) {
          return;  // something wrong
        }
      } else {
        // skip main header parsing (re-use)
        tile_hndr.restart(start_SOD);
      }
    }

    incoming_data_len += size;
    if (POS && MH == 0 && !is_parsing_failure && tile_hndr.is_ready()) {
      ACTION(parse, PID);
    }

    if (marker) {  // when EOC comes
      if (!is_parsing_failure && tile_hndr.is_ready()) {
        ACTION(flush);
      }
      trunc_frames += is_parsing_failure;
      total_frames++;

      save_j2c(total_frames, this->incoming_data, incoming_data_len);
      log_close();
      is_parsing_failure = 0;

      // printf("%d bytes allocated\n", get_bytes_allocated());
    }
  }
};
}  // namespace j2k

#endif  // FRAME_HANDLER_HPP
