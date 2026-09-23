# 0.1.18 on a MacBook Pro and an ASUS GX10, 23 September 2026

Contributed report, not a maintainer measurement. It repeats the correctness checks from the [installation guide](install.md#5-verify-rdma-before-using-it), the README latency command, and the bandwidth recipe from the [17 September report](validation-2026-09-17.md#reproducing-the-bandwidth-shape) on a different Mac and a different GB10 peer. Everything below was measured on one link, on one boot of each machine, on 23 September 2026 between 19:03 and 19:24 UTC. The Mac booted at 18:59:06 UTC.

## Test setup

| | Mac side | Peer side |
|---|---|---|
| Machine | MacBook Pro, Apple M5 Max, 128 GB (`Mac17,6`) | ASUS Ascent GX10 (GB10) |
| OS | macOS 27.0 build `26A428`, SIP disabled, Reduced Security, `rdma_ctl` enabled | Ubuntu 24.04.5 LTS, kernel `7.0.0-1019-nvidia` |
| NIC | ConnectX-5 Ex `15b3:1019` in an OWC Mercury Helios 5S | ConnectX-7 (`vendor_part_id` 4129), firmware `28.45.4028` |
| Host path | PCIe Gen4 x4, 16 GT/s (System Information); the driver's `MCDMAPCIePath` readout in `ioreg` shows max payload 128 bytes and max read request 512 bytes at the card | |
| Interface | `mcrdma3` / `rdma_mcrdma3` | `enp1s0f1np1` / `rocep1s0f1`, GID index 1 (RoCE v2) |

Physical link: one NVIDIA QSFP112 DAC, negotiated 100GBASE-CR4 with RS-FEC on both ends. Ethernet MTU 9000 on both ends. On the peer, NetworkManager was told to stop managing the port before the MTU, address and neighbor steps, because its DHCP retries otherwise undo them. No switch.

A second card was attached to the Mac during these runs: a ConnectX-4 Lx `15b3:1015` dual-port adapter on a separate Thunderbolt port. The driver also bound it (`mcrdma0`, `mcrdma1`). It had no cable and carried no traffic. The checker listed seven RDMA devices: three of Apple's own and four native ConnectX ports (two per card; `mcrdma2` is the uncabled second CX-5 port). It reported `native_cx5=4`, one active port and no errors.

## Build identity

Source: this repository at `7192192`, built on the Mac at 17:36 UTC with Xcode 27.0 (`27A266a`) and the macOS 27.0 SDK. The peer binaries were built from the same commit with GCC 13.3.0.

| Item | Identity |
|---|---|
| Kernel extension `org.mcdma.cx5.native` 0.1.18 | UUID `6BB0406E-E8D6-3965-A610-EC6F788AEFC7` |
| Provider `libmcdma-rdmav34.so` (installed copy = build copy) | SHA-256 `fc33ceb1d2e0af31b223b059b267ffea78afef40e145c81920a5f588110194fd` |
| Mac `native-verbs-peer` | SHA-256 `a9c4072965990e974195f80c57d7def2930cae2c103bc60e8fcaf60a94bea1fb` |
| Mac `mcdma-bw` | SHA-256 `de919c4906b62cb29a847c1a26b8fe965f054569edfe6f20ff0b9b97a5057d0f` |
| Mac `cx5-native-check` | SHA-256 `d87910f148655b16c3c694bde248aa7af9ed7d4fbe80061f8b2bc5ad821ed092` |
| Peer `verbs-peer` (GCC 13.3.0) | SHA-256 `d51058fa10e038aadcc3801dfd35bd9fdf4dd3ba8b7f992eb66b54349b0896a3` |
| Peer `mcdma-bw` (GCC 13.3.0) | SHA-256 `64d3b2f93022c506faefb12b2134e3bb79894d34fe4589f1d73ed41e34aeb8b9` |

## Correctness

`cx5-native-check --require-gid` passed on `rdma_mcrdma3` (port active, RoCE v2 link-local GID, no errors). Then `tools/native_cross_host.py` with 4 KiB payloads and RC path MTU 4096, one arm at a time, in the documented order:

| Arm | CQ map / user post / BlueFlame | Mode markers | Mac WRITE | Mac READ | GX10 WRITE | GX10 READ |
|---|---|---|---|---|---|---|
| Kernel posting | 0 / 0 / 0 | kernel, as requested | verified | verified | verified | verified |
| Direct posting | 2 / 1 / 0 | CQ mapped, user post confirmed | verified | verified | verified | verified |
| BlueFlame-64 | 2 / 1 / 64 | user post and BlueFlame confirmed, `uar_wc=1` | verified | verified | verified | verified |

Each "verified" is 4096 bytes checked by the runner. No arm fell back to a different posting mode.

## Latency

The README command, BlueFlame-64, RC path MTU 1024, 1000 timed operations per verb, **Metal keepalive off**. Each configuration ran twice with fresh output files. These are completion times for one operation at queue depth one, not one-way wire latency.

| Payload | Operation | Run 1 median / p99, µs | Run 2 median / p99, µs | README Studio median, µs (1) |
|---|---|---|---|---:|
| 4 KiB | Mac WRITE | 9.96 / 19.75 | 9.83 / 16.96 | 7.625 |
| 4 KiB | Mac READ | 8.02 / 11.17 | 7.75 / 11.50 | 6.042 |
| 4 KiB | GX10 WRITE | 3.17 / 3.33 | 3.12 / 4.10 | 3.680 (Spark) |
| 4 KiB | GX10 READ | 6.54 / 7.09 | 6.62 / 7.09 | 5.536 (Spark) |
| 1 KiB | Mac WRITE | 8.88 / 14.08 | 8.50 / 15.08 | not published |
| 1 KiB | Mac READ | 6.42 / 11.38 | 6.50 / 11.42 | not published |
| 1 KiB | GX10 WRITE | 2.46 / 2.83 | 2.66 / 3.30 | not published |
| 1 KiB | GX10 READ | 5.60 / 6.32 | 6.13 / 7.15 | not published |

(1) Copied from the README headline table, not re-measured: Mac Studio M3 Ultra and one DGX Spark, lab driver 0.1.17, continuous Metal keepalive on, 40 Gb/s link, pooled over three runs. Our runs differ in the Mac, the driver version (0.1.18), the keepalive (off) and the link rate (100 Gb/s), so the column is context, not a matched comparison.

All 8,000 Mac samples and 8,000 GX10 samples (4 runs x 2 verbs x 1000) completed. These four runs started five to six minutes after the Mac booted. Later runs on the same boot were faster; see the next section.

### Later runs and a keepalive A/B

From 19:18 UTC, 19 to 21 minutes after boot, the same 4 KiB / MTU 1024 / BlueFlame-64 command ran nine more times: three keepalive-off and three keepalive-on runs alternated (off, on, off, on, off, on), then three more keepalive-off runs. The keepalive was `fabric-keepalive 0 small`, started 3 s before each "on" run and stopped after it. All nine runs passed. Medians, µs:

| Run | Keepalive | Mac WRITE | Mac READ | GX10 WRITE | GX10 READ |
|---|---|---:|---:|---:|---:|
| A1 | off | 7.708 | 5.542 | 3.120 | 5.760 |
| A2 | on | 7.542 | 5.667 | 3.040 | 5.664 |
| A3 | off | 7.833 | 5.958 | 3.120 | 5.856 |
| A4 | on | 7.708 | 5.875 | 3.120 | 5.536 |
| A5 | off | 7.833 | 6.042 | 3.576 | 6.144 |
| A6 | on | 7.708 | 5.958 | 3.120 | 5.744 |
| A7 | off | 7.750 | 6.000 | 3.328 | 5.808 |
| A8 | off | 7.791 | 6.021 | 3.312 | 5.808 |
| A9 | off | 7.333 | 5.500 | 3.312 | 6.032 |

On this MacBook the keepalive made no clear difference: its on-off gaps are smaller than the spread between off runs. That differs from the Studio result in [gpu-keepalive.md](gpu-keepalive.md). The comparison was not controlled: other applications were running during all nine runs (the busiest used 130 to 145% CPU, and WindowServer about 46%), and their GPU activity may already have kept the platform in the faster state. A quiet-machine A/B was not run.

The Mac-initiated medians fell by about 2 µs between the 19:04 runs and these. The cause was not isolated; time since boot is the only recorded difference. For reference, the Studio's 100 Gb/s single runs in [gpu-keepalive.md](gpu-keepalive.md#100-gbs-link-runs) at the same MTU and payload were 7.21 to 7.29 µs WRITE and 5.92 µs READ from the Studio with keepalive on, and 9.88 / 7.12 µs with it off (driver 0.1.17).

## Sustained bandwidth

The command from [Reproducing the bandwidth shape](validation-2026-09-17.md#reproducing-the-bandwidth-shape), unchanged apart from hosts, paths and devices: kernel posting, one RC QP, queue depth one, one 4 MiB slot, 8 GiB per trial, one warmup before each measured trial, three rounds, path MTU 4096, flag finish, 1 MiB verification budget. All 12 measured trials passed with confirmed modes, no completion errors, no sampled-byte mismatches and guards intact. Initiator rows with `warmup=0`:

| Payload direction | Initiator and operation | Three runs, Gbit/s | Median, Gbit/s | 17 Sep Studio median, Gbit/s |
|---|---|---|---:|---:|
| GX10 to MacBook | Mac READ | 50.5, 50.5, 50.5 | 50.5 | 50.5 |
| GX10 to MacBook | GX10 WRITE | 50.9, 50.9, 50.9 | 50.9 | 51.0 |
| MacBook to GX10 | Mac WRITE | 28.1, 27.3, 26.4 | 27.3 | 29.4 |
| MacBook to GX10 | GX10 READ | 26.2, 28.4, 25.9 | 26.2 | 24.2 |

The right-hand column is copied from the 17 September report for reference; it was not re-measured here. As in that report, correctness is sampled (12,288 bytes after each trial), not every transferred byte. The initiator busy-polls and used about one CPU core.

The directional shape of the 17 September Studio runs appears on this MacBook as well: about 50.5 Gbit/s into the Mac and 26 to 28 Gbit/s out of it. That points away from the Studio specifically. It does not identify the cause.

### Repeat sweep and longer runs

The same 8 GiB sweep, repeated at 19:22 UTC with a fresh output directory, gave medians of 50.5 (Mac READ), 51.0 (GX10 WRITE), 26.5 (Mac WRITE) and 26.1 (GX10 READ) Gbit/s. All 12 trials passed with the same checks.

Longer single transfers followed the 17 September follow-up: one 4 MiB slot, depth one, kernel posting, no warmup, one run per configuration unless listed twice.

| Payload direction | Initiator and operation | Total payload | Duration | Gbit/s | 17 Sep Studio, Gbit/s |
|---|---|---:|---:|---:|---:|
| GX10 to MacBook | Mac READ | 96 GiB | 16.314 s | 50.5 | 50.6 |
| MacBook to GX10 | Mac WRITE, run 1 | 64 GiB | 16.743 s | 32.8 | 29.4 |
| MacBook to GX10 | Mac WRITE, run 2 | 64 GiB | 20.110 s | 27.3 | |
| MacBook to GX10 | GX10 READ, run 1 | 64 GiB | 16.685 s | 32.9 | 24.6 |
| MacBook to GX10 | GX10 READ, run 2 | 64 GiB | 21.416 s | 25.7 | |

All passed with no completion errors, no sampled-byte mismatches, intact guards and confirmed modes. Into the Mac, every run landed at 50.5 to 51.0 Gbit/s. Out of the Mac, results ranged from 25.7 to 32.9 Gbit/s between runs of the same configuration a few minutes apart. The two 32.8 / 32.9 results were not reproduced by the repeats, so they show the spread, not a new outbound rate.

## In context: the same direction over TCP

What a Mac-to-GB10 link delivered on our bench before MCDMA, next to the RDMA medians above. The TCP rows use a different driver, protocol and MTU; they show what MCDMA replaced for us, and are not MCDMA measurements or a like-for-like comparison.

| Path, Mac to GB10-class peer | Into the Mac, Gbit/s | Out of the Mac, Gbit/s |
|---|---:|---:|
| MCDMA RDMA, this MacBook, Helios 5S + CX-5 Ex (Mac READ / Mac WRITE, first 8 GiB sweep; outbound runs ranged 25.7 to 32.9) | 50.5 | 27.3 |
| MCDMA RDMA, 17 Sep Studio report (Studio READ / Studio WRITE) | 50.5 | 29.4 |
| TCP, this MacBook, same card, cable and GX10 port under Apple's Ethernet driver, `iperf3`, 1 / 4 streams (2) | 20.7 / 21.2 | 20.0 / 28.7 |
| TCP, this MacBook over 10 GbE (QNAP QNA-T310G1T, Aquantia AQC107) to a GX10 (3) | not recorded | 9.36, 8.77 |

(2) Measured 23 September 2026 at 18:25 UTC, before installing MCDMA, 10 s per run, MTU 1500 (Apple's driver caps this port at 2034). Earlier runs the same day on the other GX10 port gave 25.9 / 29.1 out and 19.0 / 13.3 in, so single TCP runs vary by several Gbit/s.
(3) Measured 17 September 2026. The outbound figures are file copies, not `iperf3`. A separate `iperf3` run gave about 9.4 Gbit/s, but its direction was not recorded, so it is not placed in either column.

## Not covered

One link only; the second CX-5 port was not cabled. No concurrent-port runs, no process-termination matrix, no keepalive A/B on an otherwise idle machine, no GPU-buffer paths, no inference workload, and no long soak. The MacBook was on AC power, battery full, default power mode (checked with `pmset` after the runs, not controlled during them). Raw CSVs, manifests and logs are kept outside this repository because they contain addresses and memory-region keys.
