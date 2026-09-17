# Architecture

`MCDMACX5Native` owns the selected PCI function and starts the HCA through the project's command transport. A build gate checks the inspected macOS versions before using Apple's private RDMA interfaces. The Apple provider registers resources, exposes a BSD address interface and supplies the callbacks used by IORDMAFamily.

The userspace provider loads through the verbs provider mechanism. Registered host buffers use the driver's DMA mapping path. The Linux peer uses libibverbs, and the cross-host runner uses SSH only for setup metadata, control and results.

## Submission and completion paths

| Setting | Submission | Completion behavior |
|---|---|---|
| `MCDMA_USER_POST=0` | Kernel posts to the HCA | Selected CQ mode |
| `MCDMA_USER_POST=1` | Process writes its mapped work queue and UAR | Userspace reports completions; kernel consumption/checking remains batched |
| `MCDMA_CQ_MAP=0` | Unchanged | Kernel polling |
| `MCDMA_CQ_MAP=1` | Unchanged | Read-only observation skips many empty kernel polls |
| `MCDMA_CQ_MAP=2` | Unchanged | Early userspace completion reporting with a kernel-verified mirror |

The 0.1.16 candidate adds opt-in `MCDMA_USER_BF` arms `64`, `128`, `64s` and `db`, requiring direct posting and a driver-granted UAR mapping requested as write-combined. Pushes and multi-request doorbells share bank advancement under the context lock. All four arms passed the recorded 0.1.16 transfer campaign; the final effective mapping attribute and isolation/removal behavior remain unverified.

## Send requests and registrations

A send request is one 64-byte WQEBB: the control segment (with the immediate and the fence and solicited bits when requested), the RDMA segment for WRITE and READ, then either up to two scatter entries (three for a plain SEND) or an inline payload of up to 28 bytes (44 for SEND). The kernel encoder and the userspace encoder produce identical bytes and the offline suite checks that. Kernel posting validates every scatter entry against its memory region and never inlines, because the bytes live in the posting process; the userspace path gathers inline bytes itself. Every request is signalled; completions with an immediate carry the raw bits and wc flag 2.

A registration pins the range through Apple's `ib_umem_get`, walks the IOMMU mapping as device segments, and programs the memory key with the largest page size for which every segment is whole pages and the HCA address shares the process address's page offset. One key carries at most 990 translation entries, so a device-contiguous mapping of many gigabytes takes a handful of large pages while a fragmented one is refused rather than described wrongly.

## Ownership

Contexts own UAR leases; QPs and CQs retain their storage across refused destruction. Read-only CQ mappings are distinct from writable work queues. Quarantine and failed cleanup retain resources when DMA safety is uncertain.

When a process exits, Apple's core destroys its objects in cleanup rounds, frees its own wrappers whether or not a destroy callback was refused, and then deallocates the context. On that deallocation the provider turns every object the core abandoned into an orphan: the core pointers are dropped so a later process whose wrappers land at the same addresses is never mistaken for the dead one, the driver-owned MR wrappers are freed, the CQ live words are invalidated for any mapping still held, and a reclaim is attempted at once and again whenever a new context opens. The kernel log records `context closed with N undestroyed objects` for such exits. The offline suite replays a forced teardown with a busy MR, refused firmware destroys and retained UAR, queue and CQ mappings. Hardware removal with mapped pages and the exact sequence of a process killed mid-operation are still only certified by hardware runs; `tools/lifecycle_torture.py` exists for that.

## Runtime settings

The registry settings `MCDMAMaxReadRequestBytes`, `MCDMARelaxedOrdering` and `MCDMAAckRequestEveryPacket` affect every user of the device. The driver now checks administrator privilege before processing `setProperties` and rejects an unprivileged caller with `kIOReturnNotPrivileged`. The development helper `mcdma-set` therefore needs to run as root. The MRRS override defaults to zero, preserving the existing setting; relaxed ordering and per-packet ACK requests default off.

## PCIe diagnostics

The driver queries MPCNT group 0 approximately once per second and exposes the returned values as `MCDMAPcie*` registry properties. Sampling uses the firmware-command lock and releases the data-path lock before issuing the command. A refused query records firmware status and syndrome in `MCDMAPcieCounters` and stops further counter queries for that attachment.

The outbound stall fields describe the percentage of the previous second spent waiting for PCIe credits, and the number of seconds above 30%, **if the firmware implements those fields**. This build does not check the MCAM `pcie_outbound_stalled` capability bit. `MCDMAPcieCounters="sampled every second"` means the register query succeeded; zero stall values do not prove the absence of stalls or establish the cause of a bandwidth limit.
