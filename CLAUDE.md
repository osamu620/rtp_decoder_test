# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A receiver/parser for High-Throughput JPEG 2000 (HTJ2K) video carried over **RTP/J2K** (RFC 9828). It pairs a small in-tree RTP receiver (`rtp_receiver.{hpp,cpp}`) with a custom JPEG 2000 codestream parser (`packet_parser/`). The goal is to ingest RTP packets, reassemble J2K frames, and feed the parsed structure into a downstream hardware decoder. There is no entropy/transform decoding here — this is the *parsing* front end only.

The design driver is **sub-codestream latency**: precinct (not frame) is the decodable unit, and the parser fires a per-precinct callback as soon as each precinct's packet header is decoded. Hold-back / drift detection / per-precinct recovery all exist in service of that goal.

## Build & run

CMake (with Ninja) is the canonical build. Build out-of-tree under `build/`:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/rtp_decoder <local_address> <local_port> [duration_s] [wait_ms] [holdback] [recv_cpu] [worker_cpu] [recv_buf_mb]
```

For a clean reconfigure use `cmake --fresh` (do not `rm -rf build` — the project's `.claude/settings.local.json` denies that pattern). For artifact-only cleanup use `cmake --build build --target clean`.

CLI args (positional, all but the first two optional):

| Arg | Default | Notes |
|-----|---------|-------|
| `local_address` | — | IPv4 to bind |
| `local_port` | — | UDP port |
| `duration_s` | forever | seconds before `Receiver::stop()` |
| `wait_ms` | 45 | deprecated, ignored |
| `holdback` | 0 | extra precinct-count cushion (vestigial since byte-queue gate; non-zero just delays parsing) |
| `recv_cpu` | 2 | pin recv thread to this CPU; `-1` = no pin |
| `worker_cpu` | 3 | pin worker thread to this CPU; `-1` = no pin |
| `recv_buf_mb` | 16 | `SO_RCVBUF` size in MB |

Defaults are tuned for ZCU102 / Cortex-A53 — see *Deployment* below.

There is a second, **standalone**, offline parser entry point in `packet_parser/main.cpp` that reads a `.j2c` file from disk. It is not wired into the top-level CMake target — `packet_parser/CMakeLists.txt` is included via `add_subdirectory` only to attach sources to `rtp_decoder`, and `main.cpp` is excluded.

## Compile-time switches that change behavior

These are not runtime flags — flip them and rebuild:

- `ENABLE_LOGGING`, `ENABLE_SAVEJ2C` in `packet_parser/utils.hpp`: write per-frame `.log` and `.j2c` files to CWD. Off by default.
- `PARSER_OVERSHOOT_INSTR` (CMake option, OFF by default): adds per-precinct byte-overshoot/drift instrumentation and recovery counters. The receiving hook prints periodic stats. See `tile_handler.hpp::OvershootStats`. Counters of interest:
  - `precincts_parsed`, `avg_prec_bytes` — throughput sanity
  - `drift_snaps`, `max_drift_bytes`, `mean_drift` — disagreement between parser end-of-precinct and authoritative resync byte (should be 0 in steady state)
  - `recoveries`, `skipped_precincts` — successful per-precinct recoveries after parse failures
  - `recover_no_signal` / `recover_bad_pid` / `recover_backward` — *why* a recovery attempt failed (each truncated frame attributable to one of these)

The parser's per-stream structures (`prec_`, `pband_`, `blk_`, tag-trees) live in a single 4 MB static arena in `packet_parser/utils.cpp` (`stackAlloc`). The arena is reset per frame via `tile_hanlder::restart` → `stackAlloc(0, 1)`. Allocations from `create()` happen once per stream; pointers remain valid across the per-frame index reset because `create()` is not re-invoked.

## Architecture

The data flow is **NIC → kernel softirq → recv thread → SPSC job queue → worker thread → frame_handler → tile_handler → per-precinct parser → user callback**. Recv and worker run on separate cores; everything from `frame_handler` down runs on the worker.

### 1. RTP reception (`rtp_receiver.{hpp,cpp}`, `receiving_hook.cpp`)

`rtp::Receiver` owns:
- a POSIX UDP socket bound to `<local_addr, local_port>` (IPv4),
- a dedicated `recv()` thread that parses the 12-byte RFC 3550 header (CSRC list and extension headers are skipped; padding is honored),
- a fixed-size jitter-buffer ring (`kRingSize = 4096` slots, `kSlotBytes = 9216`-byte slab) keyed on the 16-bit sequence number with signed-wrap arithmetic. ~38 MB total. Sized to absorb a full frame's worth of in-flight packets (~1300 at 4K@60 1.7bpp) plus jitter headroom,
- an SPSC lock-free job queue (`kJobQueueSize = kRingSize`) handing dispatched slots from recv to worker — the worker runs the user hook, parsing, and recovery without blocking recv.

Slab lifetime is **caller-controlled** (zero-copy chain): each `Frame` carries `slab_idx`, and the slot's `in_worker` atomic stays set until the consumer calls `Receiver::release_slab(idx)`. This lets `frame_handler` keep every packet's slab alive across the entire frame so the parser reads bytes directly from the slab ring rather than copying into a contiguous buffer.

Jitter-buffer policy:
- First packet sets `next_seq_`; signed `seq - next_seq_` discriminates late/duplicate (drop), in-window (park), and beyond-window.
- If the gap from `next_seq_` to an incoming seq is `>= jitter_depth_` (default 64), the head force-advances. Each advanced slot dispatches if filled, otherwise increments the lost-packet counter.
- Tunables (must be set before `start()`): `set_jitter_depth(size_t)`, `set_recv_buf_size(int)`, `set_recv_cpu(int)`, `set_worker_cpu(int)`.

Drop accounting (three independent counters — *all should be 0 in a healthy run*):
- `net_lost_packets()` — sequence-gap detection: packets that the network/kernel never delivered to the recv thread, or a slot already held a different sequence (ring alias).
- `slot_busy_drops()` — slab slot still held by worker (worker more than `kRingSize` packets behind recv).
- `queue_full_drops()` — SPSC job queue saturated at dispatch (worker more than `kJobQueueSize` jobs behind).

There is no RTCP, no SRTP, no SR/RR feedback — the receiver is purely a one-way ingestor.

### 2. RTP/J2K sub-header parsing (`frame_handler::pull_data`)

The hook strips the 12-byte RTP header (the receiver already did) and passes `(payload, payload_len - 8, marker, slab_idx)`. The 8-byte RTP/J2K payload sub-header is parsed inline:

- Byte 0 top 2 bits (`MH`): main-header indicator. `MH==0` body, `MH==1` partial main header, `MH>=2` complete main header.
- Byte 1 bit 7 (`ORDB`): RFC 9828 resync-present flag. `POS`/`PID` are only valid when `ORDB==1`.
- Big-endian dword at offset 4 (`POS_PID`): top 12 bits = `POS` (precinct byte offset within this packet), bottom 20 bits = `PID` (precinct ID).
- The J2K bytes start at `payload + 8` and are appended to the codestream chain via `cs.append_chunk(...)`. The slab index is recorded in `held_slabs_` and released back to the receiver at EOC.

`PID = c + s × num_components` (RFC 9828) — **not a sequential index**. `PID % nc` gives component, `PID / nc` gives the per-component precinct sequence number. The parser builds `crp_idx_by_pid_` (in `tile_handler.hpp::create`) so PIDs can be mapped to CRP-table positions for recovery.

State machine:
- On a Main packet (`MH >= 1`), release any stale held slabs (defensive: missed prior EOC), reset chain. If `MH >= 2` and `tile_hndr` not yet built, parse the J2K main header and call `tile_hndr.create()` to build the tile/component/resolution/precinct/codeblock tree. Subsequent frames re-use the tile structure (skip main-header parse) and just call `tile_hndr.restart(start_SOD)`.
- On a Body packet with `ORDB==1`, record `(resync_byte, PID)` in the parser's `signal_queue_` and call `tile_hndr.parse(PID)` to advance up to the byte before this packet's start.
- On the RTP marker bit (= EOC / end of J2K frame), call `tile_hndr.flush()` to drain remaining precincts, release held slabs, restart.

Failures during parsing flip `is_parsing_failure`; subsequent body packets in the frame are dropped (early-return) and counted as a "truncated" frame at EOC.

A safety cap (`held_slabs_.size() >= 3072`) guards against runaway slab consumption if EOC is persistently lost — drains the chain and waits for the next MH packet.

### 3. JPEG 2000 codestream parser (`packet_parser/`)

- `type.hpp` — central type definitions (`siz_marker`, `cod_marker`, `coc_marker`, `qcd_marker`, `dfs_marker`, `tile_`, `tcomp_`, `res_`, `prec_`, `pband_`, `blk_`, `tagtree_node`) and the **chain-based** `codestream` reader. Reads concatenate slab pointers (`chunks_`) without copying. Constants: `MAX_NUM_COMPONENTS = 3`, `MAX_DWT_LEVEL = 5`. The codestream reader handles JPEG 2000 byte-stuffing (0xFF-followed-by-bit) inline in `get_bit` / `packetheader_get_bits`. Bounds-safe accessors return 0/nullptr on OOB so a torn stream can't read past the chain end.
- `j2k_header.cpp` — `parse_main_header` walks SOC/SIZ/COD/COC/QCD/DFS markers and returns the byte offset of the first SOD.
- `tile_handler.hpp` (header-only) — owns the parsed marker structures and the tile vector. `create()` builds the full tile→component→resolution→precinct→codeblock tree once per stream and populates `crp_idx_by_pid_` for recovery. `parse(PID)` advances `crp_idx` through precincts gated by the latest signaled resync byte. `flush()` drains remaining precincts at EOC. `restart(start_SOD)` clears the signal queue and per-frame parser state.
- `j2k_packet.cpp` — `parse_one_precinct` reads one precinct's packet header (SOP/EPH handling, tag-tree decode for inclusion/zero-bitplane, layer counts, codeblock lengths) and advances the codestream by the body size. Codeblock bodies that span chain chunks are materialized via `take_contiguous(length)` (heap-owned staging in `codestream::staging_` — must NOT use `stackAlloc`, that arena is reset every frame).
- `utils.cpp` — `stackAlloc` (the static arena, no malloc fallback), and the no-op-by-default logging/save helpers.

### 4. Per-precinct recovery (`tile_handler::try_recover`)

When `parse_one_precinct` fails (truncated bytes from a dropped packet, byte-stuffing misalignment, etc.), `try_recover` snaps the parser forward to the next signaled resync point:

1. Pop signals where `byte_offset <= cur_pos` (parser already passed them).
2. If none remain → fail (`recover_no_signal`).
3. Decode `(c, s)` from the front signal's PID; map to `new_crp_idx = crp_idx_by_pid_[c][s]`.
4. If the lookup is out of bounds → fail (`recover_bad_pid`).
5. If `new_crp_idx < tile->crp_idx` → fail (`recover_backward`). The parser is already past where the next signal points, in CRP order — accepting it would mean re-firing precinct callbacks downstream out of order.
6. Otherwise: snap `cs.reset(byte_offset)`, set `crp_idx = new_crp_idx`, pop the signal, return success.

Skipped precincts (between old `crp_idx` and new) get no callback — downstream consumers see a gap. Tracked via `skipped_precincts`.

The "backward" failure mode dominated unrecovered cases on synthetic loss caused by NIC IRQ misplacement; with proper IRQ pinning the test stream produces zero failures and recovery never fires. Keep recovery in for genuine loss bursts.

## Deployment (Cortex-A53 / Petalinux on ZCU102)

This is the configuration that delivers **0 truncated frames at 4K@60 / 800+ Mbps**:

```sh
# One-time per boot (replace 51 with the actual eth0 IRQ from /proc/interrupts):
sudo sh -c 'echo 2 > /proc/irq/51/smp_affinity'   # bit 1 = CPU 1

# Then run normally:
./rtp_decoder 133.36.41.17 6000 1200 45 0 2 3 64
```

**Why**: by default Petalinux pins eth0's IRQ to CPU 0, which also handles general system work. Under 800 Mbps bursts the softirq draining the NIC RX ring runs late and the ring overflows. Drops manifest as `net=N` in this app's counters (sequence-gap detection) but **not** in `/proc/net/udp`'s drops column — the loss is below the UDP layer. Moving the IRQ to CPU 1 (idle vs CPU 0) clears it.

**CPU layout**: IRQ on 1, recv on 2, worker on 3. CPU 0 stays for system. Don't pin recv/worker to 0/1 — both have IRQ contention (memory: that combination produced 0 frames processed).

**Tunables that matter**:
- `recv_buf_mb`: 16 was lossy on real 800 Mbps even *with* IRQ pinning during early testing. 64 is comfortable. Going higher rarely helps once IRQ is fixed.
- `holdback`: leave at 0. The byte-queue gate (signal-driven) made it vestigial.
- `jitter_depth`: 64 default; not currently exposed via CLI.

**Diagnostic chain when drops appear**:
1. Check `net=` vs `busy=` vs `qfull=` in stats — only `net=` non-zero ⇒ loss before our code.
2. `cat /proc/net/udp | grep <port-in-hex>` — drops column. If 0 ⇒ loss is below UDP (NIC ring or wire).
3. `cat /proc/interrupts | grep eth` — which CPU is the NIC IRQ on? If CPU 0 only, that's almost certainly it.
4. `ip -s link show eth0` and `ethtool -S eth0 | grep -iE 'drop|err|miss|fifo'` confirm NIC-level drops.

**The IRQ affinity is not persistent across reboots.** A systemd unit or `/etc/network/if-up.d/` script would automate it; not yet done.

## Conventions

- C++17, `.clang-format` Google base, 2-space indent, 108-col limit, `SortIncludes: Never` (preserve existing include order).
- Per-frame state lives in `frame_handler` and the `tile_` instances inside `tile_handler`. Anything resembling persistent allocation should go through `stackAlloc` so it gets reset by `restart()` — heap allocations bypass that reset and will accumulate. *Exception*: `codestream::staging_` is intentionally heap-owned because it must outlive the per-frame arena reset (codeblock bodies span frames during `take_contiguous`).
- Hardware-decoder coupling is via the parsed structures (precinct/codeblock metadata in `tile_`), not via decoded samples — there is no DWT or entropy decode in this tree.
- The worker thread runs the user hook, parsing, and recovery — all on one core. Keep the hook lean and never block on user input or heavy I/O. Stats printing every ~1 s is fine; per-packet logging is not.
- Drop counters MUST remain at 0 in healthy operation. Any non-zero `busy=` or `qfull=` means the worker isn't keeping up with recv. Any non-zero `net=` with `qfull=0` means loss is upstream of our code (kernel/NIC/wire).
