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

## Ownership

Contexts own UAR leases; QPs and CQs retain their storage across refused destruction. Read-only CQ mappings are distinct from writable work queues. Quarantine and failed cleanup retain resources when DMA safety is uncertain. These checks limit what the driver attempts, but the remaining hardware removal and process-death cases are not certified by simulated tests.
