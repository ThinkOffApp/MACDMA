# GPU activity and RDMA latency

On 15 September 2026, keeping the M3 Ultra Studio GPU active with a small Metal workload repeatedly reduced RDMA completion latency between the Studio and one DGX Spark. The improvement appeared in both initiation directions, even though the timed transfers used ordinary registered host memory.

The optional [`fabric-keepalive`](../client/fabric_keepalive.swift) program submits a small Metal compute kernel repeatedly. It runs without root and does not configure the NIC, install a driver or change macOS settings. Stopping it stops the extra GPU work.

## Measured results

| Initiator | Operation | Samples | Median µs | p95 µs | p99 µs | Maximum µs |
|---|---|---:|---:|---:|---:|---:|
| Studio | WRITE | 3,000 | 7.625 | 14.000 | 16.708 | 25.250 |
| Studio | READ | 3,000 | 6.042 | 10.708 | 12.791 | 20.000 |
| Spark | WRITE | 3,000 | 3.680 | 4.320 | 4.384 | 21.312 |
| Spark | READ | 3,000 | 5.536 | 6.304 | 7.440 | 17.392 |

Conditions: M3 Ultra Studio with 256 GB, macOS 27 build `26A428`, Helios 5S over Thunderbolt 5, ConnectX-5 Ex and one DGX Spark with ConnectX-7; negotiated Ethernet rate 40 Gb/s, Ethernet MTU 9000, RDMA path MTU **1024**, 4 KiB payloads and queue depth one. The installed lab driver was **0.1.17**, with userspace completion processing, direct posting and 64-byte BlueFlame pushes. Continuous `fabric-keepalive 0 small` ran on the Studio. The probe client was built with `-O2`; per-sample WRITE filling remained enabled.

Three matching runs each recorded 1,000 samples per operation per initiator after 100 warmups. Each run passed the cross-host byte checks and confirmed the requested userspace queue and BlueFlame modes. The six CSV hashes matched those recorded by the runner. Pooling all samples reproduces the headline medians without filtering. Percentiles use nearest rank; the median averages the two central samples.

| Run | Studio WRITE µs | Studio READ µs | Spark WRITE µs | Spark READ µs |
|---|---:|---:|---:|---:|
| 1 | 7.625 | 6.083 | 3.584 | 5.376 |
| 2 | 7.666 | 6.125 | 3.744 | 5.904 |
| 3 | 7.500 | 5.959 | 3.776 | 5.872 |

These are local submission-to-observed-completion measurements on the initiator. READ pulls bytes from the peer; the arrow in the README identifies who posted the operation. Separate shared Metal/CUDA buffer correctness tests do not turn these host-buffer timings into GPU-to-GPU or inference timings. The NIC moves the payload, while CPUs still submit and observe work.

The earlier 0.1.16 headline was 10.208 / 7.875 µs from the Studio and 4.096 / 7.136 µs from the Spark. That campaign used a different path MTU and driver version, so subtracting the two tables does not isolate the keepalive's effect. Use matching off/on trials for that comparison.

## 100 Gb/s link runs

Later on 15 September 2026 the Mac–Spark link was re-cabled with Mellanox MCP1600-C001E30N passive copper cables and negotiated 100GBASE-CR4 with RS-FEC. The same installed 0.1.17 driver, BlueFlame-64 arm, `-O2` probe client and per-sample fill were used, with 1,000 samples per operation per run after 100 warmups. Every run passed four-way byte verification. These are single runs, not pooled, so the table above remains the headline. Medians are in µs; "slow" counts WRITE or READ samples above 16 µs out of 1,000.

| Run | Keepalive | Path MTU | Payload | Studio WRITE | slow | Studio READ | slow | Spark WRITE | Spark READ |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | on, small | 1024 | 4 KiB | 7.29 | 13 | 5.92 | 1 | 3.23 | 5.10 |
| 2 | on, small | 4096 | 4 KiB | 7.42 | 10 | 5.96 | 3 | 3.55 | 5.46 |
| 3 | on, small | 1024 | 4 KiB | 7.21 | 15 | 5.92 | 4 | 3.34 | 5.55 |
| 4 | on, small | 1024 | 1 KiB | 6.04 | 10 | 5.50 | 5 | 2.59 | 4.53 |
| 5 | off | 1024 | 4 KiB | 9.88 | 195 | 7.12 | 6 | 3.36 | 6.96 |
| 6 | on, small | 1024 | 4 KiB | 7.21 | 11 | 5.92 | 1 | 3.44 | 5.60 |
| 7 | on, small | 2048 | 4 KiB | 7.42 | 10 | 5.79 | 6 | 3.34 | 5.26 |

Against the 40 Gb/s keepalive-on runs recorded the same morning (4 KiB Studio WRITE 7.3–7.8, READ 5.7–6.1, Spark WRITE 3.4–3.8, Spark READ 5.4–5.9 µs), the 100 Gb/s runs were 0.1–0.4 µs lower across the board. That is roughly what serializing 4 KiB at 100 rather than 40 Gb/s saves once pipelining at path MTU 1024 hides part of it; it is not a throughput result. The keepalive-off run shows the idle slow state unchanged by the link speed: 195 of 1,000 WRITEs above 16 µs against 10–15 with the keepalive running.

## What we found, and what remains an explanation

Repeated experiments associated continuous small GPU dispatches with lower latency and fewer long completions. Heavier GPU work did not consistently give the best result. Inserting idle gaps often lost the benefit, while some burst patterns retained it, so there is no established universal idle-time threshold.

The tested PCIe maximum-read-request sizes, relaxed-ordering and per-packet ACK options did not consistently hold the faster behavior. The lab's ASPM, power-mode and user-activity experiments also failed to reproduce the continuous-small-GPU result reliably. Those outcomes apply to this hardware and experiment, not every Mac or workload.

Our working explanation is that GPU activity changes a Studio platform power/performance state that affects transactions between host memory and the Thunderbolt-attached NIC. We have not directly measured the relevant internal interconnect clock, rail or transition, so the precise mechanism remains an inference. The helper does not move the RDMA payload through Metal or make the transfer GPU-initiated.

Power and resource use matter. The headline runs did not measure incremental whole-system power, and the banner records that limitation. A later lab note reports roughly 2.75 W in GPU power telemetry while the small helper was active; that is not a matched wall-power measurement or the helper's total system cost. The helper also submits commands on the CPU and can compete with inference for GPU time. An active inference loop may already produce the favorable condition, but this needs testing rather than an assumption that the improvement comes free.

## Build and run

Complete the [RDMA installation and correctness checks](install.md) first. The current source builds **0.1.18**; the three-run set above still describes 0.1.17, and equivalent performance on 0.1.18 has not yet been measured. No driver reinstall is needed to try the helper with an already working MCDMA installation.

On the target Studio, from the repository root:

```sh
mkdir -p build
xcrun swiftc -O client/fabric_keepalive.swift -framework Metal -o build/fabric-keepalive
build/fabric-keepalive --help
build/fabric-keepalive 5 small
```

The bounded smoke check should print progress or a completion message and exit successfully. Compilation needs Apple's Swift toolchain and Metal framework, but the helper itself uses no private RDMA APIs or kernel SDK interfaces.

For a benchmark, leave this running in a separate Terminal:

```sh
build/fabric-keepalive 0 small
```

Stop with **Ctrl-C** when the run finishes. A positive duration is measured after Metal setup; `30 small` runs the dispatch loop for approximately 30 seconds. `small` uses 16,384 GPU threads and 64 arithmetic iterations per thread, with one command outstanding and no deliberate sleep between commands. `medium` uses 262,144 threads and is an optional comparison, not the headline configuration. There is no persistent setting or automatic startup service.

This public helper preserves the measured small kernel and dispatch pattern. It adds argument validation, buffer initialization, Metal error checks and monotonic duration handling, and omits experimental spin/burst modes. Its build and bounded execution checks are separate from the recorded RDMA measurements; rerun the comparison on your hardware.

## Reproduce the comparison

Use the [README latency command](../README.md#5-verify-transfers-and-check-your-speeds) with a **4096-byte payload and 1024-byte path MTU**. The path MTU is the RDMA transport payload size, distinct from Ethernet's configured 9000-byte MTU. Record the loaded driver version, binary hashes, active port and confirmed posting/completion modes.

Collect at least three matching off/on pairs, reversing their order between pairs. Stop the helper for each off run and record other GPU activity; a machine rendering a workload is not necessarily idle. Keep binaries, payload, path MTU, queue depth, warmups and sample counts unchanged. Use a fresh output filename for every run and retain all samples and correctness failures. Do not pool off and on data together or choose only the fastest trials.

The summaries report median and tail latency. Run both initiators and compare WRITE and READ separately. If inference is the goal, then measure prompt processing, token generation and power with the real model, with and without the helper. This microbenchmark establishes neither a throughput gain nor an inference speedup.

Keep raw runner JSON and logs in ignored `results/` storage because they can contain addresses and memory-region access keys. This repository publishes the helper and measurement summary; bulk research traces remain outside it.
