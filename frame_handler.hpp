//
// Created by OSAMU WATANABE on 2024/12/01.
//

#ifndef FRAME_HANDLER_HPP
#define FRAME_HANDLER_HPP
#include <cstring>

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
  size_t idx;
  siz_marker siz;
  cod_marker cod;
  coc_marker cocs[MAX_NUM_COMPONENTS];
  qcd_marker qcd;
  dfs_marker dfs;
  codestream cs;
  tile_* tiles;

 public:
  frame_handler(uint8_t* p)
      : incoming_data(p),
        incoming_data_len(0),
        idx(0),
        siz({}),
        cod({}),
        qcd({}),
        dfs({}),
        cs({}),
        tiles(nullptr) {
    cs.src = &incoming_data[0];
  }

  ~frame_handler() {
    restart_tiles(tiles, &siz);
    destroy_tiles(tiles, &siz);
    incoming_data_len = 0;
  }

  void restart() {
    restart_tiles(tiles, &siz);
    // destroy_tiles(tiles, &siz);
    incoming_data_len = 0;
  }

  void pull_data(uint8_t* data, size_t size, int MH, int marker) {
    memcpy(this->incoming_data + incoming_data_len, data, size);
    incoming_data_len += size;
    if (MH >= 2) {
      if (tiles == nullptr) {
        parse_main_header(&cs, &siz, &cod, cocs, &qcd, &dfs);
        tiles = create_tiles(&siz, cocs, &dfs, &cs);
      } else {
        // skip main header parsing (re-use)
        tiles->buf->pos = size;
      }
    }
    if (marker) {
      std::cout << "Processed " << idx + 1 << "frames" << std::endl;
      char buf[128];
      snprintf(buf, 128, "x%05lu.j2c", idx);
      FILE* fp = fopen(buf, "wb");
      fwrite(this->incoming_data, 1, incoming_data_len, fp);
      fclose(fp);
      process();
      restart();
      idx++;
    }
  }

  void process() {
    if (tiles != nullptr) {
      if (read_tiles(tiles, &siz, cocs, &dfs)) {
        restart();
      }
    }
  }
};

}  // namespace j2k

#endif  // FRAME_HANDLER_HPP
