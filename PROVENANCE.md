# Provenance and dependencies

MCDMA is developed by Ash Hart. This source snapshot contains the project's native kernel driver, userspace provider, shared encoders and regression tests; it does not bundle the earlier DriverKit project or unrelated firmware research.

The implementation depends on Apple SDK headers, Apple's installed IORDMAFamily/librdma interfaces and a Linux rdma-core/libibverbs peer. Those dependencies are not covered by this repository's Apache-2.0 grant and are not redistributed here.

The project records public mlx5/NVIDIA command and queue formats, and build-specific observations of Apple's RDMA interfaces. Public interface names and ABI declarations are compatibility information; they are not a claim that MCDMA owns those interfaces. Apple SDKs, kernel collections, extracted Apple executables and vendor firmware are not included.

Public references used in this work include [Apple XNU](https://github.com/apple-oss-distributions/xnu), [rdma-core](https://github.com/linux-rdma/rdma-core) and NVIDIA's adapter programming documentation. No MelonDMA source is intentionally included in this release preparation. Preserve any third-party copyright notices if new externally sourced material is introduced, and record its origin and license before merging it.

The source inventory and detailed publication audit are maintained outside the release tree. This document records the declared origin and dependencies; it is not a completed independent copyright or legal audit of every line.
