# 0.1.18 on a MacBook Pro and an ASUS GX10, 23 September 2026

Contributed report, not a maintainer measurement. It repeats the correctness checks from the [installation guide](install.md#5-verify-rdma-before-using-it), the README latency command, and the bandwidth recipe from the [17 September report](validation-2026-09-17.md#reproducing-the-bandwidth-shape) on a different Mac and a different GB10 peer. Everything below was measured on one link, on one boot of each machine, on 23 September 2026 between 19:03 and 19:07 UTC.

## Test setup

| | Mac side | Peer side |
|---|---|---|
| Machine | MacBook Pro, Apple M5 Max, 128 GB (`Mac17,6`) | ASUS Ascent GX10 (GB10) |
| OS | macOS 27.0 build `26A428`, SIP disabled, Reduced Security, `rdma_ctl` enabled | Ubuntu 24.04.5 LTS, kernel `7.0.0-1019-nvidia` |
| NIC | ConnectX-5 Ex `15b3:1019` in an OWC Mercury Helios 5S | ConnectX-7 (`vendor_part_id` 4129), firmware `28.45.4028` |
| Host path | PCIe Gen4 x4, 16 GT/s; the card reports max payload 128 bytes, max read request 512 bytes | |
| Interface | `mcrdma3` / `rdma_mcrdma3` | `enp1s0f1np1` / `rocep1s0f1`, GID index 1 (RoCE v2) |

Physical link: one NVIDIA QSFP112 DAC, negotiated 100GBASE-CR4 with RS-FEC on both ends. Ethernet MTU 9000 on both ends. On the peer, NetworkManager was told to stop managing the port before the MTU, address and neighbor steps, because its DHCP retries otherwise undo them. No switch.

A second card was attached to the Mac during these runs: a ConnectX-4 Lx `15b3:1015` dual-port adapter on a separate Thunderbolt port. The driver also bound it (`mcrdma0`, `mcrdma1`). It had no cable and carried no traffic. The checker reported four native devices, one active port and no errors.

## Build identity

Source: this repository at `7192192`, built on the Mac with Xcode 27.0 (`27A266a`) and the macOS 27.0 SDK. The binaries were built from a branch that differs from `7192192` only in `docs/install.md` and `rpc/rpcd_connect.c` ([PR #5](https://github.com/ashhart/MCDMA/pull/5)), which the driver, provider and benchmark clients do not use.

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

| Payload | Operation | Run 1 median / p99, µs | Run 2 median / p99, µs |
|---|---|---|---|
| 4 KiB | Mac WRITE | 9.96 / 19.75 | 9.83 / 16.96 |
| 4 KiB | Mac READ | 8.02 / 11.17 | 7.75 / 11.50 |
| 4 KiB | GX10 WRITE | 3.17 / 3.33 | 3.12 / 4.10 |
| 4 KiB | GX10 READ | 6.54 / 7.09 | 6.62 / 7.09 |
| 1 KiB | Mac WRITE | 8.88 / 14.08 | 8.50 / 15.08 |
| 1 KiB | Mac READ | 6.42 / 11.38 | 6.50 / 11.42 |
| 1 KiB | GX10 WRITE | 2.46 / 2.83 | 2.66 / 3.30 |
| 1 KiB | GX10 READ | 5.60 / 6.32 | 6.13 / 7.15 |

All 8,000 Mac samples and 8,000 GX10 samples (4 runs x 2 verbs x 1000) completed. The README's historical table (Mac Studio, 0.1.17, keepalive on) is not directly comparable: the Mac, the driver version and the keepalive state all differ. Keepalive-on runs on this MacBook were not taken.

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

## For comparison: TCP on the same link, before installation

Earlier the same day, the same card, enclosure, cable and GX10 port ran `iperf3` under Apple's built-in Ethernet driver (MTU 1500, its maximum there is 2034): 20.7 / 21.2 Gbit/s GX10 to MacBook and 20.0 / 28.7 Gbit/s MacBook to GX10, with 1 / 4 TCP streams, 10 s each. These are TCP results from a different driver and MTU, listed only as a baseline; they are not MCDMA measurements.

## Not covered

One link only; the second CX-5 port was not cabled. No concurrent-port runs, no process-termination matrix, no keepalive-on runs, no GPU-buffer paths, no inference workload, and no long soak. The MacBook was on AC power, battery full, default power mode (checked with `pmset` after the runs, not controlled during them). Raw CSVs, manifests and logs are kept outside this repository because they contain addresses and memory-region keys.
