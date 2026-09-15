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

## What we found, and what remains an explanation

Repeated experiments associated continuous small GPU dispatches with lower latency and fewer long completions. Heavier GPU work did not consistently give the best result. Inserting idle gaps often lost the benefit, while some burst patterns retained it, so there is no established universal idle-time threshold.

The tested PCIe maximum-read-request sizes, relaxed-ordering and per-packet ACK options did not consistently hold the faster behavior. The lab's ASPM, power-mode and user-activity experiments also failed to reproduce the continuous-small-GPU result reliably. Those outcomes apply to this hardware and experiment, not every Mac or workload.

Our working explanation is that GPU activity changes a Studio platform power/performance state that affects transactions between host memory and the Thunderbolt-attached NIC. We have not directly measured the relevant internal interconnect clock, rail or transition, so the precise mechanism remains an inference. The helper does not move the RDMA payload through Metal or make the transfer GPU-initiated.

Power and resource use matter. The headline runs did not measure incremental whole-system power, and the banner records that limitation. A later lab note reports roughly 2.75 W in GPU power telemetry while the small helper was active; that is not a matched wall-power measurement or the helper's total system cost. The helper also submits commands on the CPU and can compete with inference for GPU time. An active inference loop may already produce the favorable condition, but this needs testing rather than an assumption that the improvement comes free.

## Build and run

Complete the [RDMA installation and correctness checks](install.md) first. The public installer currently builds **0.1.16**; this helper has no dependency on the extra 0.1.17 lab knobs, but identical performance on 0.1.16 has not been measured by the three-run set above. No driver reinstall is needed to try the helper with an already working MCDMA installation.

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
