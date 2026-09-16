# MCDMA and MelonDMA

Both projects give an Apple Silicon Mac RDMA over a Mellanox ConnectX card in a Thunderbolt PCIe enclosure, with a Linux RoCE peer. They differ in the card they drive, the verbs they implement, how they attach to macOS, what they have measured, and their licences. This page lists those differences with sources. It assigns no scores.

MelonDMA's statements were checked on 2026-09-15 against its own published files:

- [README](https://github.com/b-ostrov/MelonDMA/blob/main/README.md)
- [CHANGELOG.md](https://github.com/b-ostrov/MelonDMA/blob/main/CHANGELOG.md)
- [src/dext/SUPPORTED_HARDWARE.md](https://github.com/b-ostrov/MelonDMA/blob/main/src/dext/SUPPORTED_HARDWARE.md)

MCDMA's statements come from this repository and its measurement notes. Nothing below reproduces MelonDMA's measurements; they are its author's reported figures, quoted with what they measure.

## Hardware

| | MCDMA | MelonDMA |
|---|---|---|
| Card driven | ConnectX-5 Ex, MCX516A-CDAT, PCI `15b3:1019` | ConnectX-4 Lx PF, PCI `15b3:1015` |
| Other ConnectX generations | Not tested; universal ConnectX support is not established ([README](../README.md#scope-and-limits)) | `SUPPORTED_HARDWARE.md` lists ConnectX-5, -6, -7 and -8 as "not supported or not claimed"; the published DEXT personality matches only `0x101515b3` |
| Host link | Thunderbolt 5, PCIe Gen4 x4, 128-byte maximum payload per the driver's PCIe-path readout | PCIe Gen3 x4 over Thunderbolt, per its CHANGELOG |
| Network link | 100GBASE-CR4 with RS-FEC, validated 2026-09-15; earlier runs at 40 Gb/s | 40 Gb/s to the Spark peer, per its README and CHANGELOG |
| Peer | One DGX Spark, ConnectX-7 | One DGX Spark, ConnectX-7 |

Neither project drives the other's card. MelonDMA's hardware matrix excludes the ConnectX-5; MCDMA has not been tried on a ConnectX-4 Lx.

## Verbs surface

MelonDMA's README describes RC, UC and UD queue pairs, shared receive queues, RC atomics, MSI-X completion delivery, `SEND_WITH_INV`, a real `ibv_qp_to_qp_ex`, async events and a 44-symbol verbs export. Its CHANGELOG records FLR recovery and re-initialisation cycles.

MCDMA's provider, from [`native/user_provider.c`](../native/user_provider.c) and [`native/apple_provider.cpp`](../native/apple_provider.cpp) as of 2026-09-15 (offline-tested; the rows marked † await a hardware run):

| Area | MCDMA today |
|---|---|
| Queue pair types | RC only; no SRQ, UC, UD or XRC |
| Work requests per QP | At most 31 send and 31 receive, one 64-byte WQEBB each |
| Scatter/gather | Up to 2 entries on a send request (3 for SEND without immediate); exactly 1 on a receive |
| Send opcodes | RDMA WRITE, RDMA WRITE with immediate †, SEND, SEND with immediate †, RDMA READ; no atomics, no invalidate |
| Send flags | `IBV_SEND_SIGNALED` is required on every request; `IBV_SEND_FENCE` and `IBV_SEND_SOLICITED` are honoured †; `IBV_SEND_INLINE` up to 28 bytes for RDMA and 44 for SEND, on the userspace posting path only † |
| Receive completions | SEND, SEND with immediate † and WRITE with immediate † (reported as `IBV_WC_RECV_RDMA_WITH_IMM`); invalidate formats are consumed but reported as an error |
| Completion queues | At most 31 entries; no completion channel, no vector, no async events |
| Memory registration | Up to 1 TiB requested; what a registration can describe depends on the IOMMU mapping: the driver picks the largest MKey page size (4 KiB to 1 GiB) the mapping allows and fits at most 990 translation entries per key † |
| Objects | 256 per device, 64 per context; 136 mappings per context |

No UC or UD queue pairs, no atomics, no async event delivery and no completion channel exist in MCDMA. Its provider is not a general-purpose verbs implementation yet; the README's [scope section](../README.md#scope-and-limits) lists the open lifecycle cases as well.

## Latency

| | MCDMA | MelonDMA |
|---|---|---|
| Published figure | WRITE 7.625 µs, READ 6.042 µs from the Studio; WRITE 3.680 µs, READ 5.536 µs from the Spark | p50 8.67 µs, p99 12.08 µs in its CHANGELOG summary table; later CHANGELOG entries record the same tool at p50 5.96–6.25 µs |
| What it measures | 4 KiB RDMA operations between the two machines, submission to observed completion on the initiator, queue depth one, path MTU 1024 | 64-byte operations on one host, `mlx_rtt_bench`: a single RC QP writing to its own registered buffer through the card; its source describes this as "not a two-machine round trip" |
| Samples | 3,000 per operation per initiator across three runs, all retained; tails published ([note](gpu-keepalive.md)) | Not stated with the figure |
| Conditions | Lab driver 0.1.17, continuous Metal keepalive on the Studio, 40 Gb/s link at the time | Direct UAR and directly polled CQ, per its CHANGELOG summary table |

The two figures measure different things and should not be placed on one axis. MCDMA has published cross-machine figures with every sample retained; MelonDMA's published latency figure is a single-host loopback. MCDMA's figure depends on the GPU keepalive described in its [measurement note](gpu-keepalive.md); without it the 4 KiB WRITE median was 9.88 µs in the 100 Gb/s idle run.

## Bandwidth

| | MCDMA | MelonDMA |
|---|---|---|
| Published benchmark | No validated sustained-performance result. A queue-depth, large-transfer runner is included in 0.1.18 | Single-QP RDMA WRITE 20.1–20.7 Gbit/s at 1 MiB; 8-QP aggregate 21.16 Gbit/s; Mac ← Spark 23.0 Gbit/s; all in its CHANGELOG |
| Stated ceiling | Thunderbolt 5 PCIe Gen4 x4 tunnel; no measured ceiling published | Self-declared practical ceiling of 25–28 Gbit/s on the PCIe Gen3 x4 tunnel, per its CHANGELOG |
| Workload observation | A 4,057 MiB KV cache pulled with 2 MiB RDMA READs in 0.90 s, 38 Gbit/s, during the [disaggregated inference run](disaggregated-inference.md); not a benchmark | — |

MCDMA's 38 Gbit/s figure came from one workload with file-staged plumbing and no queue-depth sweep. It is not comparable to a benchmark and is not presented as one.

## GPU memory

| | MCDMA | MelonDMA |
|---|---|---|
| Validated path | Metal shared buffers on the Studio and CUDA mapped host allocations on the Spark, registered in place; 24 GPU-produced, GPU-verified RDMA operations across both directions with 48 deliberate negative checks ([README](../README.md#integration-with-inference-engines)) | "Metal shared-memory" listed among the gates passed on the ConnectX-4 Lx in `SUPPORTED_HARDWARE.md` |
| Not established | Direct registration of `cudaMalloc` device allocations (fails with `EFAULT` on the tested Spark); Metal private buffers | GPUDirect and Metal `.private` memory, listed as not supported in `SUPPORTED_HARDWARE.md` |
| Inference workload | One Qwen3-4B prefill/decode hand-off, [documented with its limitations](disaggregated-inference.md) | Its README links an inference client guide; no result from it was checked for this page |

Both projects stop at the same NVIDIA-documented Spark limitation: integrated-GPU device allocations are not registrable by PCIe devices, so both use host-mapped allocations on the Spark side.

## Attachment to macOS

| | MCDMA | MelonDMA |
|---|---|---|
| Form | Kernel extension that registers with Apple's IORDMAFamily and a provider for Apple's verbs stack | DriverKit system extension (`PCIDriverKit`) that replaces `AppleEthernetMLX5` as the card's owner, with its own `libibverbs`-compatible layer and IOUserClient shim, per its README |
| Consequence | Apple's own `ibv_devinfo` and `librdma` see the device as `rdma_mcrdmaN` with no shim; the build depends on inspected private Apple interfaces and is gated to specific macOS builds ([architecture](architecture.md)) | Applications link MelonDMA's verbs layer rather than Apple's; the card leaves Apple's Ethernet driver while MelonDMA is active |
| Recovery | Process death with outstanding direct work and hot removal with mapped pages are unverified ([README](../README.md#scope-and-limits)) | FLR recovery and re-initialisation cycles recorded in its CHANGELOG |
| Installation | Reduced Security, user-managed kernel extensions, `rdma_ctl enable` and, for the ad-hoc-signed development build, `csrutil disable` ([install](install.md)) | Its README states Apple has not granted the PCI/UserClient entitlements and describes SIP-off development as a stopgap |

Neither project installs with System Integrity Protection enabled today.

## Licence

MCDMA is Apache-2.0 and records its provenance in [PROVENANCE.md](../PROVENANCE.md); no MelonDMA source is intentionally included. MelonDMA is GPL-2.0 and describes itself as a port of GPL-2.0-only mlx5 code.

## What this page does not do

It does not reproduce MelonDMA's measurements, rank the projects, or predict either project's future support. MelonDMA's files may have changed since 2026-09-15; check the linked sources before relying on a row.
