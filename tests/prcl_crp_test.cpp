// PRCL / PCRL precinct-progression order test.
//
// Drives the codestream parser front end on a raw JPEG 2000 codestream (.j2c/.jhc)
// and exercises prepare_precinct_structure() — the function that builds the
// component/resolution/precinct (CRP) walk the parser uses as packet identity.
//
// Two modes:
//   dump   : print the built CRP order, one "c r p" triple per line, to stdout.
//            Used to diff against an authoritative reference (e.g. the OpenHTJ2K
//            encoder's packet emission order).
//   parse  : additionally run the full tile_handler::flush() parse path over the
//            whole codestream and assert (a) every precinct parses cleanly and
//            (b) the precincts fire back, in order, identical to the built CRP walk.
//            Exits non-zero on any failure.
//
// Usage:  prcl_crp_test <codestream.j2c> [dump|parse]   (default: dump)
//
// This is test-harness wiring for the PRCL progression work; it touches no hot path.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "type.hpp"
#include "j2k_header.hpp"
#include "tile_handler.hpp"
#include "utils.hpp"

namespace {

std::vector<uint8_t> read_file(const char *path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    std::fprintf(stderr, "error: cannot open %s\n", path);
    std::exit(2);
  }
  const std::streamsize n = f.tellg();
  f.seekg(0);
  std::vector<uint8_t> buf(static_cast<size_t>(n));
  f.read(reinterpret_cast<char *>(buf.data()), n);
  return buf;
}

// Collects (c, r, p) triples fired by tile_handler's precinct-ready callback.
std::vector<crp_status> g_parsed;
void on_precinct(void * /*user*/, const prec_ * /*pp*/, uint8_t c, uint8_t r, uint16_t p) {
  g_parsed.push_back({c, r, p});
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <codestream.j2c> [dump|parse]\n", argv[0]);
    return 2;
  }
  const std::string mode = (argc > 2) ? argv[2] : "dump";

  std::vector<uint8_t> bytes = read_file(argv[1]);

  codestream cs;
  cs.append_chunk(bytes.data(), bytes.size());

  tile_handler th;
  const uint32_t start_SOD = parse_main_header(&cs, th.get_siz(), th.get_cod(), th.get_cocs(),
                                               th.get_qcd(), th.get_dfs());
  if (start_SOD == 0) {
    std::fprintf(stderr, "error: parse_main_header failed (not a valid codestream?)\n");
    return 2;
  }
  if (!th.create(&cs)) {
    std::fprintf(stderr, "error: create() failed — unsupported progression (%zu/%zu tiles built)\n",
                 th.get_num_tiles(), (size_t)0);
    th.restart(0);  // exercise the per-frame cleanup the real pipeline runs at EOC on a
                    // rejected stream — must not crash on a partially-built (multi-tile) state
    std::fprintf(stderr, "cleanup (restart) survived a rejected stream\n");
    return 3;
  }

  const siz_marker *siz = th.get_siz();
  const coc_marker *cocs = th.get_cocs();
  const dfs_marker *dfs = th.get_dfs();
  const bool dfs_present = (dfs->Idfs != 0);
  std::fprintf(stderr,
               "[info] %s: Csiz=%u layers=%u PO=%u start_SOD=%u dfs=%s NL={%u,%u,%u} crp=%zu\n",
               argv[1], siz->Csiz, cocs[0].num_layers, cocs[0].progression_order, start_SOD,
               dfs_present ? "yes" : "no", cocs[0].NL,
               siz->Csiz > 1 ? cocs[1].NL : 0u, siz->Csiz > 2 ? cocs[2].NL : 0u,
               th.get_tile_crp().size());

  const std::vector<crp_status> &crp = th.get_tile_crp();

  if (mode == "dump") {
    for (const auto &e : crp) {
      std::printf("%u %u %u\n", e.c, e.r, e.p);
    }
    return 0;
  }

  if (mode == "parse") {
    // Dump the built order to stdout too (so a single run can both gate and parse).
    for (const auto &e : crp) std::printf("%u %u %u\n", e.c, e.r, e.p);

    th.set_precinct_callback(on_precinct, nullptr);
    g_parsed.clear();
    const int ret = th.flush();  // cs is positioned at start_SOD after parse_main_header

    bool ok = true;
    if (ret != EXIT_SUCCESS) {
      std::fprintf(stderr, "[FAIL] flush() returned %d — a precinct failed to parse\n", ret);
      ok = false;
    }
    if (g_parsed.size() != crp.size()) {
      std::fprintf(stderr, "[FAIL] parsed %zu precincts, expected %zu (CRP walk truncated)\n",
                   g_parsed.size(), crp.size());
      ok = false;
    }
    const size_t n = g_parsed.size() < crp.size() ? g_parsed.size() : crp.size();
    for (size_t i = 0; i < n; ++i) {
      if (g_parsed[i].c != crp[i].c || g_parsed[i].r != crp[i].r || g_parsed[i].p != crp[i].p) {
        std::fprintf(stderr,
                     "[FAIL] precinct %zu fired as (c=%u r=%u p=%u) but CRP walk has (c=%u r=%u p=%u)\n",
                     i, g_parsed[i].c, g_parsed[i].r, g_parsed[i].p, crp[i].c, crp[i].r, crp[i].p);
        ok = false;
        break;
      }
    }
    if (ok) {
      std::fprintf(stderr, "[PASS] %zu precincts parsed cleanly, fired in CRP order\n", crp.size());
      return 0;
    }
    return 1;
  }

  std::fprintf(stderr, "error: unknown mode '%s' (want dump|parse)\n", mode.c_str());
  return 2;
}
