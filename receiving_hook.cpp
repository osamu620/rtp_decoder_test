#include <uvgrtp/lib.hh>

#include <thread>
#include <iostream>
#include <iomanip>

/* There are two main ways of getting received RTP frames from uvgRTP.
 * This example demonstrates the usage of hook function to receive RTP frames.
 *
 * The advantage of using a hook function is minimal CPU usage and delay between
 * uvgRTP receiving the frame and application processing the frame. When using
 * the hook method, the application must take care that it is not using the hook
 * function for heavy processing since this may block RTP frame reception.
 *
 * Hook based frame reception is generally recommended for most serious applications,
 * but there can be situations where polling method is better, especially if performance
 * is not a huge concern or if there needs to be tight control when the frame is
 * received by the application.
 *
 * This example only implements the receiving, but it can be used together with the
 * sending example to test the functionality.
 */

#include <frame_handler.hpp>

alignas(8) static uint8_t incoming_data[1024 * 4096];

struct params_t {
  j2k::frame_handler *frame_handler;
  size_t total_frames;
  uint32_t last_timetamp;
};

void rtp_receive_hook(void *arg, uvgrtp::frame::rtp_frame *frame);
void cleanup(uvgrtp::context &ctx, uvgrtp::session *sess, uvgrtp::media_stream *receiver);

void print_help(char *cmd) {
  std::cout << "Usage:" << cmd << " address port duration(s) wait(ms)" << std::endl;
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    print_help(argv[0]);
    return EXIT_FAILURE;
  }
  std::cout << "Starting HTJ2K RTP/J2K packet decoder" << std::endl;

  const char *LOCAL_ADDRESS = argv[1];
  const uint16_t LOCAL_PORT = static_cast<uint16_t>(std::stoi(argv[2]));
  int64_t TIME_S            = INT64_MAX;
  if (argc > 3) {
    TIME_S = std::stoi(argv[3]);
  }
  // How long this example will run
  const auto RECEIVE_TIME_S = std::chrono::seconds(TIME_S);

  [[maybe_unused]] size_t RECEIVER_WAIT_TIME_MS = 45;
  if (argc > 4) {
    RECEIVER_WAIT_TIME_MS = static_cast<size_t>(std::stoi(argv[4]));
  }

  j2k::frame_handler frame_handler(incoming_data);
  size_t total_frames = 0;
  params_t params{};
  params.frame_handler = &frame_handler;
  params.total_frames  = total_frames;
  params.last_timetamp = 0;

  uvgrtp::context ctx;
  uvgrtp::session *sess          = ctx.create_session(LOCAL_ADDRESS);
  int flags                      = RCE_RECEIVE_ONLY;  // | RCE_FRAGMENT_GENERIC;
  uvgrtp::media_stream *receiver = sess->create_stream(LOCAL_PORT, RTP_FORMAT_GENERIC, flags);
  // receiver->configure_ctx(RCC_UDP_RCV_BUF_SIZE, 16384 * 1024);
  // receiver->configure_ctx(RCC_MTU_SIZE, 4096);
  // receiver->configure_ctx(RCC_RING_BUFFER_SIZE, 2048 * 1024);
  /* Receive hook can be installed and uvgRTP will call this hook when an RTP frame is received
   *
   * This is a non-blocking operation
   *
   * If necessary, receive hook can be given an argument and this argument is supplied to
   * the receive hook every time the hook is called. This argument could a pointer to application-
   * specfic object if the application needs to be called inside the hook
   *
   * If it's not needed, it should be set to nullptr */
  if (!receiver || receiver->install_receive_hook(&params, rtp_receive_hook) != RTP_OK) {
    std::cerr << "Failed to install RTP reception hook";
    cleanup(ctx, sess, receiver);
    return EXIT_FAILURE;
  }

  std::cout << "Waiting incoming packets for " << RECEIVE_TIME_S.count() << " s" << std::endl;

  std::this_thread::sleep_for(RECEIVE_TIME_S);  // lets this example run for some time

  cleanup(ctx, sess, receiver);

  return EXIT_SUCCESS;
}

void rtp_receive_hook(void *arg, uvgrtp::frame::rtp_frame *frame) {
  // constexpr ptrdiff_t OFFSET = 0;
  // // std::cout << "Received RTP frame" << std::endl;
  //
  // /* Now we own the frame. Here you could give the frame to the application
  //  * if f.ex "arg" was some application-specific pointer
  //  *
  //  * arg->copy_frame(frame) or whatever
  //  *
  //  * When we're done with the frame, it must be deallocated manually */
  // uint8_t *pp = frame->payload + OFFSET;
  // uint32_t RTP_header_val;
  // //
  // int MH = pp[0] >> 6;
  // // int TP      = (pp[0] >> 3) & 0x7;
  // // int ORDH    = pp[0] & 0x7;
  // // int RES     = ORDH;
  // // int P       = pp[1] >> 7;
  // // int XTRAC   = (pp[1] >> 4) & 0x7;
  // // int QUAL    = XTRAC;
  // // tmp         = __builtin_bswap32(*(uint32_t *)(pp));
  // // int PTSTAMP = (tmp >> 8) & 0xFFF;
  // // int ESEQ    = pp[3];
  // // int R       = pp[4] >> 7;
  // // int S       = (pp[4] >> 6) & 1;
  // // int C       = (pp[4] >> 5) & 1;
  // // int RSVD    = (pp[4] >> 1) & 0x7;
  // // int PRIMS   = pp[5];
  // // int TRANS   = pp[6];
  // // int MAT     = pp[7];
  //
  // RTP_header_val = __builtin_bswap32(*(uint32_t *)(pp + 4));
  //
  // if (MH == 0) {
  //   // // BODY
  //   // printf("RES = %d, QUAL = %d, ESEQ = %d, POS = %d, PID = %d, s = %d, c = %d, seq = %d\n", RES,
  //   QUAL,
  //   //        ESEQ, POS, PID, PID / 3, PID % 3, ESEQ * 65536 + frame->header.seq);
  // } else {
  //   // HEADER
  //   // printf("timestamp = %d\n", frame->header.timestamp);
  // }

  const auto p           = static_cast<struct params_t *>(arg);
  j2k::frame_handler *fh = p->frame_handler;
  fh->pull_data(frame->payload, frame->payload_len - 8, frame->header.marker);

  size_t last_processed_frames = fh->get_total_frames();
  if (p->last_timetamp == 0) {
    p->last_timetamp = frame->header.timestamp;
  }
  if (frame->header.timestamp >= p->last_timetamp + 45000) {
    std::cout << "Elapsed time: " << std::left << std::setw(8) << std::right << std::fixed
              << std::setprecision(3)
              << (fh->get_cumlative_time_then_reset() / 1000.0
                  / ((last_processed_frames - p->total_frames)))
              << " [ms/frame], ";

    std::cout << "Processed frames: " << std::setw(7) << last_processed_frames << ", " << std::setw(7)
              << std::fixed << std::setprecision(4)
              << (1000.0 * (last_processed_frames - p->total_frames) / fh->get_duration()) << " fps, "
              << "trunc J2K frames = " << std::setw(5) << fh->get_trunc_frames() << ", "
              << "lost RTP frames = " << std::setw(5) << fh->get_lost_frames() << std::endl;
    // printf("Elapsed time: %-15.3lf[ms], ",
    //        fh->get_cumlative_time_then_reset() / 1000.0 / ((last_processed_frames - p->total_frames)));
    // printf("Processed frames: %5zu, %7.4f fps, trunc J2K frames = %3lu, lost RTP frames = %3lu\n",
    //        last_processed_frames, 1000.0 * (last_processed_frames - p->total_frames) /
    //        fh->get_duration(), fh->get_trunc_frames(), fh->get_lost_frames());
    p->total_frames  = last_processed_frames;
    p->last_timetamp = frame->header.timestamp;
  }
  (void)uvgrtp::frame::dealloc_frame(frame);
}

void cleanup(uvgrtp::context &ctx, uvgrtp::session *sess, uvgrtp::media_stream *receiver) {
  if (receiver) {
    sess->destroy_stream(receiver);
  }

  if (sess) {
    /* Session must be destroyed manually */
    ctx.destroy_session(sess);
  }
}
