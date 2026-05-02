#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>

#include <frame_handler.hpp>
#include "rtp_receiver.hpp"

#ifdef __linux__
  #include <arpa/inet.h>
  #include <ifaddrs.h>
  #include <net/if.h>
  #include <netinet/in.h>
  #include <cctype>
  #include <cstdint>
  #include <cstring>
  #include <fstream>
  #include <sstream>
  #include <string>
#endif

struct params_t {
  j2k::frame_handler *frame_handler;
  rtp::Receiver *receiver;
  size_t total_frames;
  uint32_t last_timetamp;
};

#ifdef __linux__
namespace {

// Find the network interface name for a bind address. For 0.0.0.0 returns the
// first non-loopback IPv4 interface. Empty string on failure.
std::string find_iface_for_bind(const char *bind_addr) {
  in_addr target{};
  const bool match_any = (std::strcmp(bind_addr, "0.0.0.0") == 0);
  if (!match_any && inet_pton(AF_INET, bind_addr, &target) != 1) return "";

  ifaddrs *ifs = nullptr;
  if (getifaddrs(&ifs) != 0) return "";
  std::string result;
  for (ifaddrs *it = ifs; it != nullptr; it = it->ifa_next) {
    if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
    if (it->ifa_flags & IFF_LOOPBACK) continue;
    auto *a = reinterpret_cast<sockaddr_in *>(it->ifa_addr);
    if (match_any || a->sin_addr.s_addr == target.s_addr) {
      result = it->ifa_name;
      break;
    }
  }
  freeifaddrs(ifs);
  return result;
}

// Match `iface` as a whole token (preceded by start-of-line/non-alnum and
// followed by end-of-line/comma/whitespace).
bool line_mentions_iface(const std::string &line, const std::string &iface) {
  size_t pos = 0;
  while ((pos = line.find(iface, pos)) != std::string::npos) {
    const bool start_ok = (pos == 0 || !std::isalnum(static_cast<unsigned char>(line[pos - 1])));
    const size_t end    = pos + iface.size();
    const bool end_ok =
        (end == line.size() || line[end] == ',' || std::isspace(static_cast<unsigned char>(line[end])));
    if (start_ok && end_ok) return true;
    pos = end;
  }
  return false;
}

// Parse /proc/interrupts for the IRQ number serving `iface`. -1 on failure.
int find_irq_for_iface(const std::string &iface) {
  std::ifstream f("/proc/interrupts");
  if (!f) return -1;
  std::string line;
  while (std::getline(f, line)) {
    if (!line_mentions_iface(line, iface)) continue;
    std::istringstream iss(line);
    int irq    = -1;
    char colon = 0;
    if ((iss >> irq >> colon) && colon == ':') return irq;
  }
  return -1;
}

// Read /proc/irq/N/smp_affinity and return the low 64 bits of the mask.
// 0 on failure (also a valid "no CPUs" value but the kernel rejects writing 0,
// so any live IRQ has at least one bit set).
uint64_t read_smp_affinity(int irq) {
  std::ostringstream path;
  path << "/proc/irq/" << irq << "/smp_affinity";
  std::ifstream f(path.str());
  if (!f) return 0;
  std::string s;
  if (!std::getline(f, s)) return 0;
  // Format: comma-separated 32-bit hex chunks, high to low (e.g. "00000002" or
  // "00000000,00000002"). Strip commas, take the low 16 hex chars (64 bits).
  std::string hex;
  for (char c : s)
    if (c != ',') hex.push_back(c);
  if (hex.size() > 16) hex = hex.substr(hex.size() - 16);
  uint64_t mask = 0;
  for (char c : hex) {
    int v = -1;
    if (c >= '0' && c <= '9')
      v = c - '0';
    else if (c >= 'a' && c <= 'f')
      v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      v = c - 'A' + 10;
    else
      return 0;
    mask = (mask << 4) | static_cast<uint64_t>(v);
  }
  return mask;
}

// Print a one-line banner showing where the NIC IRQ is pinned, and a warning
// if it overlaps recv/worker. All steps fail silently — this is informational.
void check_nic_irq_affinity(const char *bind_addr, int recv_cpu, int worker_cpu) {
  const std::string iface = find_iface_for_bind(bind_addr);
  if (iface.empty()) return;
  const int irq = find_irq_for_iface(iface);
  if (irq < 0) return;
  const uint64_t mask = read_smp_affinity(irq);
  if (mask == 0) return;

  std::ostringstream cpus;
  bool first = true;
  for (int c = 0; c < 64; ++c) {
    if (mask & (1ULL << c)) {
      if (!first) cpus << ",";
      cpus << c;
      first = false;
    }
  }

  std::cout << "NIC " << iface << ": IRQ " << irq << " on CPU " << cpus.str();

  const bool recv_conflict   = (recv_cpu >= 0 && (mask & (1ULL << recv_cpu)));
  const bool worker_conflict = (worker_cpu >= 0 && (mask & (1ULL << worker_cpu)));
  if (recv_conflict || worker_conflict) {
    std::cout << "  <-- WARNING: shares CPU with "
              << (recv_conflict && worker_conflict ? "recv+worker"
                                                   : (recv_conflict ? "recv" : "worker"))
              << "; pin to a different CPU (see scripts/pin-nic-irq.sh) for high-bitrate stability";
  }
  std::cout << std::endl;
}

}  // namespace
#else
static void check_nic_irq_affinity(const char *, int, int) {}
#endif

static void rtp_receive_hook(void *arg, const rtp::Frame &frame);

void print_help(char *cmd) {
  std::cout
      << "Usage: " << cmd
      << " address port [duration(s)] [wait(ms)] [holdback] [recv_cpu] [worker_cpu] [recv_buf_mb]"
      << std::endl;
  std::cout << "  duration:    seconds to listen (default: forever)" << std::endl;
  std::cout << "  wait(ms):    deprecated, ignored (kept for arg-position compatibility)" << std::endl;
  std::cout << "  holdback:    parser hold-back precincts (default 0; vestigial since byte-queue gate)"
            << std::endl;
  std::cout << "  recv_cpu:    CPU to pin recv thread (default 2; -1 = no pinning)" << std::endl;
  std::cout << "  worker_cpu:  CPU to pin worker thread (default 3; -1 = no pinning)" << std::endl;
  std::cout << "  recv_buf_mb: SO_RCVBUF in megabytes (default 16)" << std::endl;
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

  int recv_cpu    = 2;
  int worker_cpu  = 3;
  int recv_buf_mb = 16;
  if (argc > 6) recv_cpu = std::stoi(argv[6]);
  if (argc > 7) worker_cpu = std::stoi(argv[7]);
  if (argc > 8) recv_buf_mb = std::stoi(argv[8]);

  // Receiver lives longer than frame_handler so frame_handler can call back into the
  // receiver during destruction or final EOC (C++ destroys locals in reverse order).
  rtp::Receiver receiver;
  receiver.set_recv_cpu(recv_cpu);
  receiver.set_worker_cpu(worker_cpu);
  receiver.set_recv_buf_size(recv_buf_mb * 1024 * 1024);

  j2k::frame_handler frame_handler;
  if (argc > 5) {
    const uint32_t HOLDBACK = static_cast<uint32_t>(std::stoul(argv[5]));
    frame_handler.set_parse_holdback(HOLDBACK);
  }
  std::cout << "Parse hold-back: " << frame_handler.get_parse_holdback() << " precincts" << std::endl;
  std::cout << "Recv pin: " << (recv_cpu < 0 ? "off" : ("CPU " + std::to_string(recv_cpu)))
            << ", Worker pin: " << (worker_cpu < 0 ? "off" : ("CPU " + std::to_string(worker_cpu)))
            << ", SO_RCVBUF: " << recv_buf_mb << " MB" << std::endl;
  check_nic_irq_affinity(LOCAL_ADDRESS, recv_cpu, worker_cpu);

  // Wire slab-release: frame_handler holds slabs across each frame (zero-copy chain
  // parsing) and releases them via this callback at EOC.
  frame_handler.set_release_slab_callback(
      [](void *r, size_t idx) { static_cast<rtp::Receiver *>(r)->release_slab(idx); }, &receiver);

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
  fh->pull_data(frame.payload, frame.payload_len - 8, frame.marker, frame.slab_idx);

  size_t last_processed_frames = fh->get_total_frames();
  if (p->last_timetamp == 0) {
    p->last_timetamp = frame.timestamp;
  }
  if (frame.timestamp >= p->last_timetamp + 45000) {
    const double frames_in_window = static_cast<double>(last_processed_frames - p->total_frames);
    std::cout << "Elapsed time: " << std::left << std::setw(8) << std::right << std::fixed
              << std::setprecision(3)
              << (fh->get_cumlative_time_then_reset() / 1000.0 / frames_in_window) << " [ms/frame], ";

    std::cout << "Processed frames: " << std::setw(7) << last_processed_frames << ", " << std::setw(7)
              << std::fixed << std::setprecision(4)
              << (1000.0 * frames_in_window / fh->get_duration()) << " fps, "
              << "trunc J2K frames = " << std::setw(5) << fh->get_trunc_frames() << ", "
              << "RTP drops: net=" << std::setw(5) << p->receiver->net_lost_packets()
              << " busy=" << std::setw(5) << p->receiver->slot_busy_drops()
              << " qfull=" << std::setw(5) << p->receiver->queue_full_drops() << std::endl;
#ifdef PARSER_OVERSHOOT_INSTR
    const auto os = fh->get_overshoot_stats();
    const double avg_prec_bytes =
        os.precincts_parsed
            ? static_cast<double>(os.sum_precinct_bytes) / static_cast<double>(os.precincts_parsed)
            : 0.0;
    const double avg_drift =
        os.snaps_with_drift
            ? static_cast<double>(os.sum_drift_bytes) / static_cast<double>(os.snaps_with_drift)
            : 0.0;
    std::cout << "  Parser: precincts=" << os.precincts_parsed
              << " avg_prec_bytes=" << std::fixed << std::setprecision(1) << avg_prec_bytes
              << " drift_snaps=" << os.snaps_with_drift << " max_drift_bytes=" << os.max_drift_bytes
              << " mean_drift=" << std::fixed << std::setprecision(1) << avg_drift
              << " recoveries=" << os.recoveries << " skipped_precincts=" << os.skipped_precincts
              << std::endl;
    if (os.failed_parses) {
      std::cout << "  Failures: count=" << os.failed_parses
                << " recover_fail: no_sig=" << os.recover_no_signal
                << " bad_pid=" << os.recover_bad_pid << " backward=" << os.recover_backward
                << " last={c=" << os.last_fail_c << " r=" << os.last_fail_r << " p=" << os.last_fail_p
                << " crp_idx=" << os.last_fail_crp_idx << " src=" << os.last_fail_src_pos << "}"
                << std::endl;
    }
    fh->reset_overshoot_stats();
#endif
    p->total_frames  = last_processed_frames;
    p->last_timetamp = frame.timestamp;
  }
}
