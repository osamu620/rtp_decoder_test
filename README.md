# rtp_decoder_test

Receiver and parser for **High-Throughput JPEG 2000 (HTJ2K)** video carried over **RTP/J2K** (RFC 9828). Ingests RTP packets, reassembles the JPEG 2000 codestream, parses precinct/codeblock metadata, and exposes a per-precinct callback for a downstream hardware decoder.

This is the *parsing* front end — there is no DWT or entropy decoding in this tree.

Target platform: **Xilinx ZCU102 (Cortex-A53 quad-core, Petalinux)** at **4K @ 60 fps, 800+ Mbps**.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

For instrumented builds (per-precinct stats, recovery counters):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPARSER_OVERSHOOT_INSTR=ON
cmake --build build
```

For a clean reconfigure use `cmake --fresh`.

## Run

```sh
./build/rtp_decoder <addr> <port> [duration_s] [wait_ms] [holdback] [recv_cpu] [worker_cpu] [recv_buf_mb]
```

| Arg | Default | Notes |
|-----|---------|-------|
| `addr` | — | IPv4 to bind |
| `port` | — | UDP port |
| `duration_s` | forever | seconds to listen |
| `wait_ms` | 45 | deprecated, ignored |
| `holdback` | 0 | precinct-count cushion (vestigial; leave at 0) |
| `recv_cpu` | 2 | recv thread pin; `-1` = no pin |
| `worker_cpu` | 3 | worker thread pin; `-1` = no pin |
| `recv_buf_mb` | 16 | `SO_RCVBUF` size in MB |

Typical ZCU102 invocation for 4K@60 800 Mbps:

```sh
./build/rtp_decoder 133.36.41.17 6000 1200 45 0 2 3 64
```

## Deployment on ZCU102 — required before high-bitrate runs

By default Petalinux pins the eth0 IRQ to CPU 0, which is also where general system work runs. Under 800 Mbps bursts the softirq draining the NIC RX ring runs late and the ring overflows. The drops are invisible at the UDP socket layer (`/proc/net/udp` shows `drops=0`) but show up as `net=N` in this app's counters.

**Move the eth0 IRQ to CPU 1 before running**:

```sh
# Find the IRQ number for eth0:
cat /proc/interrupts | grep eth
# Pin it to CPU 1 (bit 1 = CPU 1):
sudo sh -c 'echo 2 > /proc/irq/<IRQ_NUM>/smp_affinity'
```

CPU layout on the board: **IRQ on 1, recv on 2, worker on 3**. CPU 0 stays for the system. Don't pin recv/worker to 0 or 1 — both have IRQ contention.

Setting is **not persistent across reboot**. Re-apply each boot, or add a systemd unit / `/etc/network/if-up.d/` script.

### Verified result

With the IRQ on CPU 1 and the default CLI args above:

| Metric | Value |
|--------|-------|
| Frames processed | 1170 (over 19.5 s) |
| Frame rate | 60.00 fps |
| Truncated frames | **0** |
| Net loss (sequence-gap) | **0** |
| Slot-busy drops | 0 |
| Job-queue-full drops | 0 |
| Per-frame parse time | ~6.4 ms (~38% of 16.67 ms budget) |

Without the IRQ fix (default CPU 0): ~2% truncation rate, ~40 packets/min lost.

## Diagnosing drops

Three independent counters print every ~1 s:

- `net=N` — sequence-gap detection. Packets lost before the recv thread saw them. Could be NIC ring, wire, or sender.
- `busy=N` — slab slot still held by worker (worker fell more than 4096 packets behind). If non-zero, the worker is the bottleneck.
- `qfull=N` — SPSC job queue saturated. Same root cause as `busy=N`.

If only `net=N` is non-zero, the loss is **upstream of our code**. In that case, faster parsing won't help — investigate:

```sh
# 1. UDP socket drops (port 6000 = 0x1770):
cat /proc/net/udp | awk 'NR==1 || $2 ~ /:1770$/'
# If "drops" column is 0, loss is below UDP — check NIC.

# 2. NIC IRQ placement:
cat /proc/interrupts | grep eth

# 3. NIC-level drops:
ip -s link show eth0
ethtool -S eth0 | grep -iE 'drop|err|miss|fifo|overrun'
```

If `busy=` or `qfull=` is non-zero, the worker thread is the bottleneck — check parser performance.

## Repository layout

```
rtp_receiver.{hpp,cpp}    Recv thread, slab ring (4096 × 9216 bytes), SPSC job queue, worker
receiving_hook.cpp        CLI entry point, wires receiver to frame_handler
frame_handler.hpp         Per-packet RTP/J2K sub-header parsing, chain assembly, EOC handling
packet_parser/
  type.hpp                Marker structs, chain-based codestream reader (zero-copy)
  j2k_header.cpp          SOC/SIZ/COD/COC/QCD/DFS marker walk
  tile_handler.hpp        Tile/component/resolution/precinct/codeblock tree;
                          per-precinct parse loop; per-precinct recovery (try_recover)
  j2k_packet.cpp          Single-precinct packet header decode
  utils.{hpp,cpp}         4 MB static arena (stackAlloc), logging stubs
  main.cpp                Standalone offline parser (not built by top-level CMake)
```

## Compile-time flags

| Flag | Where | Default | Effect |
|------|-------|---------|--------|
| `PARSER_OVERSHOOT_INSTR` | CMake option | OFF | Per-precinct stats, recovery counters |
| `ENABLE_LOGGING` | `packet_parser/utils.hpp` | off | Per-frame `.log` files in CWD |
| `ENABLE_SAVEJ2C` | `packet_parser/utils.hpp` | off | Per-frame `.j2c` dumps in CWD |

`PARSER_OVERSHOOT_INSTR` is the one to enable when investigating performance or loss — it adds an extra line per stats interval with `precincts=N avg_prec_bytes=X recoveries=N skipped_precincts=N` and a `Failures: count=N recover_fail: no_sig=N bad_pid=N backward=N last={...}` line on truncations. Overhead is negligible.

## Standards

- **RFC 3550** — RTP, RTP fixed header
- **RFC 9828** — RTP payload format for sub-codestream-latency JPEG 2000

The CLAUDE.md was originally written referencing RFC 9670 (frame-level JPEG 2000); the actual design driver is RFC 9828's sub-codestream latency model where the precinct, not the frame, is the decodable unit.
