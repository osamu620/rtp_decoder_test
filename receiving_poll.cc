#include <uvgrtp/lib.hh>

#include <iostream>
#include <thread>

#include "frame_handler.hpp"

/* This example demostrates using polling to receive RTP frames. Polling in
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

// parameters of this example. You may change these to reflect you network environment
constexpr uint16_t LOCAL_PORT = 8080;

constexpr char LOCAL_ADDRESS[] = "127.0.0.1";

// How long this example will run
constexpr auto RECEIVE_TIME_MS      = std::chrono::milliseconds(30000);
constexpr int RECEIVER_WAIT_TIME_MS = 45;

static uint8_t incoming_data[1024 * 2048];

void process_frame(uvgrtp::frame::rtp_frame *frame, j2k::frame_handler *fh);

int main(void) {
  std::cout << "Starting uvgRTP RTP receive hook example" << std::endl;

  j2k::frame_handler frame_handler(incoming_data);
  uvgrtp::context ctx;
  uvgrtp::session *sess = ctx.create_session(LOCAL_ADDRESS);
  int flags             = RCE_RECEIVE_ONLY;

  uvgrtp::media_stream *receiver = sess->create_stream(LOCAL_PORT, RTP_FORMAT_H265, flags);

  if (receiver) {
    std::cout << "Start receiving frames for " << RECEIVE_TIME_MS.count() << " ms" << std::endl;
    auto start = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() - start < RECEIVE_TIME_MS) {
      /* You can specify a timeout for the operation and if a frame is not received
       * within that time limit, pull_frame() returns a nullptr
       *
       * The parameter tells how long time a frame is waited in milliseconds */
      uvgrtp::frame::rtp_frame *frame = receiver->pull_frame(RECEIVER_WAIT_TIME_MS);

      if (frame) {
        process_frame(frame, &frame_handler);
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

void process_frame(uvgrtp::frame::rtp_frame *frame, j2k::frame_handler *fh) {
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
  int POS = (((uint16_t)pp[4])<<4) + (pp[5] >> 4);
  int PID = (((uint32_t)(pp[5] & 0x0F)) << 16) + (((uint32_t)pp[6]) << 8) + pp[7];
  int TRANS   = pp[6];
  int MAT     = pp[7];

  if (MH == 0) {
    // body
    // printf("RES = %d, QUAL = %d, ESEQ = %d, POS = %d, PID = %d, seq = %d\n", RES, QUAL, ESEQ, POS, PID, ESEQ * 65536 + frame->header.seq);
  }

  fh->pull_data(frame->payload + 12, frame->payload_len - 12, MH, frame->header.marker);


  /* When we receive a frame, the ownership of the frame belongs to us and
   * when we're done with it, we need to deallocate the frame */
  (void)uvgrtp::frame::dealloc_frame(frame);
}
