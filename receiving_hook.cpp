#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>

#include <frame_handler.hpp>
#include "rtp_receiver.hpp"

struct params_t {
  j2k::frame_handler *frame_handler;
  rtp::Receiver *receiver;
  size_t total_frames;
  uint32_t last_timetamp;
};

static void rtp_receive_hook(void *arg, const rtp::Frame &frame);

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
  const auto RECEIVE_TIME_S = std::chrono::seconds(TIME_S);

  [[maybe_unused]] size_t RECEIVER_WAIT_TIME_MS = 45;
  if (argc > 4) {
    RECEIVER_WAIT_TIME_MS = static_cast<size_t>(std::stoi(argv[4]));
  }

  j2k::frame_handler frame_handler;
  rtp::Receiver receiver;
  params_t params{};
  params.frame_handler = &frame_handler;
  params.receiver      = &receiver;
  params.total_frames  = 0;
  params.last_timetamp = 0;

  if (!receiver.start(LOCAL_ADDRESS, LOCAL_PORT, &params, rtp_receive_hook)) {
    std::cerr << "Failed to start RTP receiver" << std::endl;
    return EXIT_FAILURE;
  }

  std::cout << "Waiting incoming packets for " << RECEIVE_TIME_S.count() << " s" << std::endl;

  std::this_thread::sleep_for(RECEIVE_TIME_S);

  receiver.stop();

  return EXIT_SUCCESS;
}

static void rtp_receive_hook(void *arg, const rtp::Frame &frame) {
  const auto p           = static_cast<struct params_t *>(arg);
  j2k::frame_handler *fh = p->frame_handler;
  fh->pull_data(frame.payload, frame.payload_len - 8, frame.marker);

  size_t last_processed_frames = fh->get_total_frames();
  if (p->last_timetamp == 0) {
    p->last_timetamp = frame.timestamp;
  }
  if (frame.timestamp >= p->last_timetamp + 45000) {
    std::cout << "Elapsed time: " << std::left << std::setw(8) << std::right << std::fixed
              << std::setprecision(3)
              << (fh->get_cumlative_time_then_reset() / 1000.0
                  / ((last_processed_frames - p->total_frames)))
              << " [ms/frame], ";

    std::cout << "Processed frames: " << std::setw(7) << last_processed_frames << ", " << std::setw(7)
              << std::fixed << std::setprecision(4)
              << (1000.0 * (last_processed_frames - p->total_frames) / fh->get_duration()) << " fps, "
              << "trunc J2K frames = " << std::setw(5) << fh->get_trunc_frames() << ", "
              << "lost RTP frames = " << std::setw(5) << p->receiver->lost_packets() << std::endl;
    p->total_frames  = last_processed_frames;
    p->last_timetamp = frame.timestamp;
  }
}
