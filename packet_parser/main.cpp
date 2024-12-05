#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "j2k_header.hpp"
#include "j2k_tile.hpp"
#include "j2k_packet.hpp"
#include "tile_handler.hpp"

#define ERR_PRINTF_EXIT(...)      \
  do {                            \
    fprintf(stderr, __VA_ARGS__); \
    exit(EXIT_FAILURE);           \
  } while (0)

/**
 * @brief get file siz3e
 * @param[in] in_fname string for file name
 * @retval filesize
 */
static long get_read_file_size(const char *in_fname) {
  FILE *fp;
  long file_size;
  struct stat stbuf;
  int fd;

  fd = open(in_fname, O_RDONLY);
  if (fd == -1) ERR_PRINTF_EXIT("cant open file : %s.\n", in_fname);

  fp = fdopen(fd, "rb");
  if (fp == NULL) ERR_PRINTF_EXIT("cant open file : %s.\n", in_fname);

  if (fstat(fd, &stbuf) == -1) ERR_PRINTF_EXIT("cant get file state : %s.\n", in_fname);

  file_size = stbuf.st_size;

  if (fclose(fp) != 0) ERR_PRINTF_EXIT("cant close file : %s.\n", in_fname);

  return file_size;
}

#define MAXSIZE (1024 * 2000)
static uint8_t incoming_stream[MAXSIZE];

int main(int argc, char *argv[]) {
  // check args
  if (argc < 2) {
    perror("USAGE: ./a.out j2cname\n");
    return EXIT_FAILURE;
  }
  const size_t length = get_read_file_size(argv[1]);

  FILE *fp;
  if ((fp = fopen(argv[1], "rb")) == NULL) {
    printf("%s is not found\n", argv[1]);
    return EXIT_FAILURE;
  }

  if (length != fread(incoming_stream, sizeof(uint8_t), length, fp)) {
    perror("length mismatch\n");
    return EXIT_FAILURE;
  }

  struct rusage usage;
  struct timeval t0, t1;

  getrusage(RUSAGE_SELF, &usage);
  t0 = usage.ru_utime;

  // init
  codestream buf(incoming_stream);

  // IO_map iomap = {0};
  tile_hanlder TH;

  uint32_t pos_SOD;
  pos_SOD = parse_main_header(&buf, TH.get_siz(), TH.get_cod(), TH.get_cocs(), TH.get_qcd(), TH.get_dfs());
  TH.create(&buf);

  // print_SIZ(&siz, cocs, dfs);

  for (int i = 0; i < 1; i++) {
    TH.read();
    TH.restart(pos_SOD);
    // if (read_tile(tiles, cocs, &dfs)) {
    //   printf("read tile error\n");
    //   restart_tiles(tiles, &siz);
    //   // destroy_tiles(tiles, &siz);
    //   buf.pos  = 0;
    //   buf.bits = 0;
    //   return EXIT_FAILURE;
    // }
    // restart_tiles(tiles, &siz);
  }
  // destroy_tiles(tiles, &siz);

  getrusage(RUSAGE_SELF, &usage);
  t1 = usage.ru_utime;

#ifdef DEBUG
  printf("%d bytes allocated\n", get_bytes_allocated());
#endif
  printf("%lf [ms/frame]\n", (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) * 1.0E-6);
  return EXIT_SUCCESS;
}
