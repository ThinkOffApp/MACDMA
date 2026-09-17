# 0.1.18 validation, 17 September 2026

Sustained host-memory RDMA now has measured results, alongside recovery checks after process termination during traffic. The later permission and counter update passed fresh four-way correctness checks and three longer bandwidth runs. These measurements do not establish a speedup over 0.1.17 or GPU-buffer throughput.

## Build identities

Both builds use the 0.1.18 version label, so the label alone does not identify the tested source.

| Build | Kernel UUID | Provider SHA-256 |
|---|---|---|
| Initial 0.1.18 acceptance build | `18ABCBFB-57CF-3577-A123-0BC4BC5D5BD3` | `f2962636bd67ce45375d654adfda4134767f7680077faf61753e768d3b9bf6ee` |
| Permission and PCIe-counter update | `0B3074D1-560B-3FBC-954B-B9F327965DC7` | `18fb492ee38a2663a2d8187b04b412c81c5e7bda87c122d332c0918457ef0eca` |

The provider source is unchanged between these builds. Binary hashes can change when rebuilt, including through the recorded library install name; compare your installed binary against your own build record. The source update adds the administrator check and MPCNT diagnostics described in the [architecture note](architecture.md#pcie-diagnostics).

## Test setup and measurement boundary

Mac Studio M3 Ultra, 256 GB, macOS 27 build `26A428`, OWC Mercury Helios 5S, ConnectX-5 Ex MCX516A-CDAT, and DGX Spark peers with ConnectX-7. Each tested Ethernet link negotiated 100 Gb/s. The Studio's PCIe path reported Gen4 x4, maximum payload 128 bytes and default maximum read request 512 bytes. Ethernet MTU was 9000 and RDMA path MTU 4096.

The bandwidth tests used kernel posting, one RC QP, queue depth one, a repeatedly reused 4 MiB host-memory slot plus guard space, and a flag completion protocol. Throughput is transferred payload bytes divided by the initiator's measured data-loop time, including the finish-marker completion for WRITE. It excludes registration, setup, payload initialization and post-transfer verification. These are sustained transfers through a reused region, not a stream of uniquely allocated model tensors.

The lab executable was named `mcdma-region-check`, based on `benchmarks/mcdma_bw.c` with an additional full-slot verification option. That option was not exercised in these bandwidth runs: the 1 MiB verification budget selected three 4 KiB windows, checking **12,288 bytes after each trial**, plus guards. No completion errors, sampled-byte mismatches or guard failures were recorded. This is sampled correctness, not verification of every transferred byte. The separate initial 4 MiB acceptance tests checked the full payload.

The initiator busy-polls completions and used about one CPU core in these runs. A WRITE responder also spins on the finish flag; the NIC still moves the payload by DMA. RDMA does not mean the benchmark uses no CPU.

## Single-link results

The initial build ran each operation three times, with one warmup trial before each measured trial and 8 GiB transferred per trial; the runner reversed operation order on alternate rounds. Only initiator rows with `warmup=0` contribute to this table.

| Payload direction | Initiator and operation | Three runs, Gbit/s | Median, Gbit/s |
|---|---|---|---:|
| Spark to Studio | Studio READ | 50.5461, 50.5647, 50.5422 | 50.5 |
| Spark to Studio | Spark WRITE | 50.9571, 50.9547, 50.9598 | 51.0 |
| Studio to Spark | Studio WRITE | 29.4043, 29.5449, 27.9162 | 29.4 |
| Studio to Spark | Spark READ | 24.1592, 24.7728, 24.1093 | 24.2 |

After the permission and counter update was installed, the following longer runs reproduced the directional difference. These are **one run per configuration**, without a warmup, not three-run medians.

| Payload direction | Initiator and operation | Total payload | Measured duration | Gbit/s |
|---|---|---:|---:|---:|
| Spark to Studio | Studio READ | 96 GiB | 16.302 s | 50.6 |
| Studio to Spark | Studio WRITE | 64 GiB | 18.676 s | 29.4 |
| Studio to Spark | Spark READ | 64 GiB | 22.342 s | 24.6 |

The updated build also passed byte-verified 4 KiB WRITE and READ from both initiators in kernel, direct and BlueFlame-64 modes. A non-root runtime-setting write returned `0xe00002c1`, `kIOReturnNotPrivileged`.

## Both ports at once

The initial build ran one process per Studio port against two Sparks concurrently. Each process transferred 32 GiB through one 4 MiB slot, without a warmup, once per configuration. The figures below come from the retained initiator CSV rows; aggregate is the sum of the two independently timed rates, not a measurement with a shared start/stop clock.

| Operation | Link A, Gbit/s | Link B, Gbit/s | Sum, Gbit/s |
|---|---:|---:|---:|
| Studio READ from both Sparks | 25.5943 | 25.5933 | 51.2 |
| Studio WRITE to both Sparks | 15.4051 | 15.4052 | 30.8 |
| Both Sparks READ from Studio | 12.8351 | 12.8352 | 25.7 |

Both processes completed with the sampled checks and guards intact. These bounded runs show shared card/enclosure bandwidth rather than two independent 100 Gb/s paths. They do not establish an exact hardware ceiling or general multi-client isolation.

## Process termination during traffic

On the initial build, all 12 combinations of kernel/direct/BlueFlame-64 posting, Studio initiator/responder and READ/WRITE passed recovery checks after SIGKILL. Each used one 4 MiB slot, one QP, depth one and a 64 GiB transfer. The Studio endpoint was killed about four seconds after process start.

Peer counters sampled every 100 ms showed more than 100 MB/s in the half-second preceding each kill, with timestamps aligned using the measured host clock offset. This establishes sustained traffic immediately before termination; it does not identify the precise WQE executing at the instant of the signal.

In every case the Studio stayed on the same boot, the process disappeared in 0.287 to 0.297 seconds, the checker reported two active CX5 ports with no errors, the peer endpoint exited, and a fresh four-way byte-verified transfer in the same mode passed. No new panic report appeared during the matrix.

Apple's RDMA core destroyed the resources itself in all 12 cases. The driver's fallback orphan-reclaim path did not run, so these results do not validate that fallback on hardware. Coverage was one client and one link at a time, not a long soak; the separate concurrent-port runs did not include process termination.

## PCIe counter interpretation

The updated firmware query accepted MPCNT group 0 and returned zero stall values during all three longer runs. The MCAM `pcie_outbound_stalled` capability bit has not been checked, and unsupported fields may read as zero. Those readings therefore do **not** establish that the NIC never waited for PCIe credits or identify the cause of the slower Studio-to-Spark direction.

Foreign-QPN isolation, mapped hot removal, fallback reclaim under refused destruction, larger or fragmented memory registrations and a matched 0.1.17/0.1.18 performance comparison remain open.

## Reproducing the bandwidth shape

First complete the [installation and correctness checks](install.md#5-verify-rdma-before-using-it) and set the endpoint variables from that guide. Compile `benchmarks/mcdma_bw.c` on both endpoints and set `MAC_BW` and `PEER_BW` to their executable paths, then run:

```sh
python3 benchmarks/run_bw.py \
  --mac-host "$MAC_SSH" --peer-host "$PEER_SSH" \
  --mac-bw "$MAC_BW" --peer-bw "$PEER_BW" \
  --mac-provider /usr/local/lib/rdma/libmcdma-rdmav34.so \
  --mac-checker "$MAC_CHECKER" \
  --mac-interface "$MAC_IF" --peer-interface "$PEER_IF" \
  --mac-device "$MAC_RDMA_DEVICE" --peer-device "$PEER_RDMA_DEVICE" \
  --peer-gid-index "$PEER_GID_INDEX" \
  --ops read,write --initiators mac,peer --sizes 4194304 \
  --depths 1 --qps 1 --total 8589934592 --repeats 3 --warmup 1 \
  --mtu 4096 --finish flag --verify-bytes 1048576 --timeout 120 \
  --mac-cq-map 0 --mac-user-post 0 --mac-user-bf 0 \
  --output results/bandwidth-4m
```

Preserve the manifest, CSVs and logs, require confirmed posting modes and successful verification, and select initiator rows with `warmup=0`. Raw lab evidence stays outside this repository because it includes infrastructure details and memory-region keys. The tables above were recalculated from those CSVs after verifying their manifest hashes; they summarize the retained measurements rather than publishing the raw logs.
