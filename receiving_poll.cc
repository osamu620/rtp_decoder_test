#include <uvgrtp/lib.hh>

#include <iostream>
#include <thread>

#include "frame_handler.hpp"

/* This example demonstrates using polling to receive RTP frames. Polling in
 * uvgRTP can be done with function pull_frame in media_streamer. This pull_frame
 * function can be used with or without a timeout argument. If used without a timeout
 * argument, the function will return when a frame is received or the media stream
 * is destroyed. At this point I would recommend using it with timeout and not
 * destroying the media stream since this functionality has not been verified.
 *
 * Compared to hook function, polling offers more control on frame reception,
 * but I would recommend using a hook function where possible due to reduced
 * CPU usage and latency.
 *
 * This example implements only the reception of the stream, but it can be paired
 * with the sending example to complete the demonstration.
 */

static uint8_t incoming_data[1024 * 2048];

void process_frame(uvgrtp::frame::rtp_frame *frame, j2k::frame_handler *fh, size_t &, uint32_t &);

void print_help(char *cmd) {
  std::cout << "Usage:" << cmd << " address port duration(ms) wait(ms)" << std::endl;
}

int main(int argc, char*argv[]) {
  if (argc < 3) {
    print_help(argv[0]);
    return EXIT_FAILURE;
  }
  std::cout << "Starting uvgRTP RTP receive hook example" << std::endl;

  const char * LOCAL_ADDRESS = argv[1];
  const uint16_t LOCAL_PORT = static_cast<uint16_t>(std::stoi(argv[2]));
  int64_t TIME_MS = INT64_MAX;
  if (argc > 3) {
    TIME_MS = std::stoi(argv[3]);
  }
  // How long this example will run
  const auto RECEIVE_TIME_MS = std::chrono::milliseconds(TIME_MS);
  size_t RECEIVER_WAIT_TIME_MS = 45;
  if (argc > 4) {
    RECEIVER_WAIT_TIME_MS = static_cast<size_t>(std::stoi(argv[4]));
  }

  j2k::frame_handler frame_handler(incoming_data);
  size_t total_frames      = 0;
  uint32_t last_time_stamp = 0;

  uvgrtp::context ctx;
  uvgrtp::session *sess = ctx.create_session(LOCAL_ADDRESS);
  int flags             = RCE_RECEIVE_ONLY | RCE_FRAGMENT_GENERIC;

  uvgrtp::media_stream *receiver = sess->create_stream(LOCAL_PORT, RTP_FORMAT_H266, flags);
  receiver->configure_ctx(RCC_UDP_RCV_BUF_SIZE, 8000000);
  if (receiver) {
    std::cout << "Start receiving frames for " << RECEIVE_TIME_MS.count() << " ms" << std::endl;
    auto start = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() - start < RECEIVE_TIME_MS || TIME_MS == 0) {
      /* You can specify a timeout for the operation and if a frame is not received
       * within that time limit, pull_frame() returns a nullptr
       *
       * The parameter tells how long time a frame is waited in milliseconds */
      uvgrtp::frame::rtp_frame *frame = receiver->pull_frame(RECEIVER_WAIT_TIME_MS);

      if (frame) {
        // std::cout << frame->header.timestamp << "," << last_time_stamp << std::endl;
        process_frame(frame, &frame_handler, total_frames, last_time_stamp);
      } else {
        frame_handler.countup_lost_frames();
      }
    }

    sess->destroy_stream(receiver);
  }

  if (sess) {
    /* Session must be destroyed manually */
    ctx.destroy_session(sess);
  }

  return EXIT_SUCCESS;
}

void process_frame(uvgrtp::frame::rtp_frame *frame, j2k::frame_handler *fh, size_t &total_frames,
                   uint32_t &last_time_stamp) {
  uint8_t *pp = frame->payload + 4;
  //
  int MH      = pp[0] >> 6;
  int TP      = (pp[0] >> 3) & 0x7;
  int ORDH    = pp[0] & 0x7;
  int RES = ORDH;
  int P       = pp[1] >> 7;
  int XTRAC   = (pp[1] >> 4) & 0x7;
  int QUAL = XTRAC;
  int PTSTAMP = (((uint16_t)pp[1] & 0xF) << 8) + pp[2];
  int ESEQ    = pp[3];
  int R       = pp[4] >> 7;
  int S       = (pp[4] >> 6) & 1;
  int C       = (pp[4] >> 5) & 1;
  int RSVD    = (pp[4] >> 1) & 0x7;
  int PRIMS   = pp[5];
  int POS     = (((uint16_t)pp[4]) << 4) + (pp[5] >> 4);
  int PID = (((uint32_t)(pp[5] & 0x0F)) << 16) + (((uint32_t)pp[6]) << 8) + pp[7];
  int TRANS   = pp[6];
  int MAT     = pp[7];

  if (MH == 0) {
    // body
    // printf("RES = %d, QUAL = %d, ESEQ = %d, POS = %d, PID = %d, s = %d, c = %d, seq = %d\n", RES, QUAL,
    //        ESEQ, POS, PID, PID / 3, PID % 3, ESEQ * 65536 + frame->header.seq);
  }

  fh->pull_data(frame->payload + 12, frame->payload_len - 12, MH, frame->header.marker);

  size_t last_processed_frames = fh->get_total_frames();
  if (last_processed_frames - total_frames >= 30) {
    last_time_stamp = frame->header.timestamp;

    printf("Processed frames: %5zu, %7.4f fps, trunc J2K frames = %3d, lost RTP frames = %3d\n",
           last_processed_frames, 1000.0 * (last_processed_frames - total_frames) / fh->get_duration(),
           fh->get_trunc_frames(), fh->get_lost_frames());
    // std::cout << "Processed frames = " << std::setw(5) << last_processed_frames
    //           << ", fps = " << std::setw(8)
    //           << 1000.0 * (last_processed_frames - total_frames) / fh->get_duration()
    //           << ", trunc = " << fh->get_trunc_frames() << ", lost = " << fh->get_lost_frames()
    //           << std::endl;
    total_frames = last_processed_frames;
  }

  /* When we receive a frame, the ownership of the frame belongs to us and
   * when we're done with it, we need to deallocate the frame */
  (void)uvgrtp::frame::dealloc_frame(frame);
}
