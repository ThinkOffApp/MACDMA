# MCDMA

![MCDMA connecting a Mac Studio and NVIDIA DGX Spark over Thunderbolt 5 and ConnectX-5, with measured RDMA completion times](assets/mcdma.png)

*Measured host-memory RDMA with the Studio GPU active; shared Metal/CUDA buffer correctness was verified separately.*

## Speed: one Spark ↔ one Mac Studio

**Current link: 40 Gb/s.** I am currently using the wrong cable for the intended 100GbE setup, and the Mac–Spark link is negotiating 40GbE. I have two **Mellanox Passive Copper Cable 100GbE QSFP28 to QSFP28, 1 m, MCP1600-C001E30N** replacement cables on order. I will check the negotiated rate and rerun the benchmarks once they arrive; 100GbE operation with the replacements is not yet verified, and the Thunderbolt path still limits end-to-end throughput.

| Initiator → peer | RDMA WRITE | RDMA READ |
|---|---:|---:|
| Mac Studio → Spark | **7.625 µs** | **6.042 µs** |
| Spark → Mac Studio | **3.680 µs** | **5.536 µs** |

Measured on **macOS 27, build 26A428**, with an **M3 Ultra Mac Studio, 256 GB**, and **one NVIDIA DGX Spark**, using lab driver **0.1.17**, userspace BlueFlame-64 and **continuous Metal keepalive on the Studio**. These are 4 KiB submission-to-application-observed-completion medians at queue depth one and **RDMA path MTU 1024**, pooled across three runs with 1,000 samples per operation per initiator after 100 warmups. All 12,000 measured samples are retained, including outliers.

The arrow names the initiator: a Mac-initiated READ fetches bytes from the Spark. These are registered host-memory measurements, not one-way network latency, GPU-buffer latency or inference results. WRITE / READ p95 values were **14.000 / 10.708 µs** from the Mac and **4.320 / 6.304 µs** from the Spark. The [measurement note](docs/gpu-keepalive.md) includes tails, per-run results and reproduction steps; raw traces remain outside this driver repository.

**What changed:** keeping the Studio GPU active with a small Metal workload repeatedly reduced RDMA latency in our tests. Our working explanation is a platform power/performance state associated with GPU activity; the exact internal mechanism is not measured. `fabric-keepalive` is an optional, unprivileged userspace program with no reboot or driver setting required. It consumes GPU time and power, so test its effect on your actual workload.

**Version distinction:** the measurements above used the installed lab driver 0.1.17; this repository's build and installation recipe still produce **0.1.16**. This update publishes the standalone helper, not a new driver installer. The helper uses only Metal APIs and does not require a 0.1.17-specific interface, but the same latency on 0.1.16 has not been established by this three-run set.

MCDMA is an experimental native macOS RDMA driver and userspace verbs provider for Mellanox ConnectX-5 Ex, developed by **Ash Hart**. The NICs move the payload; the CPU still submits work and observes completions.

## Integration with inference engines

MCDMA provides the RDMA driver and verbs transport. Using that transport for inference requires further integration in engines and frameworks such as **oMLX, MLX-LM and llama.cpp**, through their memory allocators and communication backends. The runtime needs to allocate transferred tensors in GPU-accessible memory that can also be registered for RDMA, retain those allocations and registrations until transfers finish, and synchronize GPU execution with transfer completion.

Separate functional tests have verified RDMA WRITE and READ in both initiation directions using Metal shared buffers and CUDA mapped host allocations. GPU kernels produced and checked the payload in the same allocations registered with RDMA, without a payload staging copy. These tests establish GPU access to the transferred memory; the latency table above measures ordinary host buffers and is not an inference or GPU-buffer benchmark. The CPU still schedules GPU work and manages RDMA operations.

Installing MCDMA alone does not connect an inference engine to this path. Direct registration of an existing `cudaMalloc` allocation still fails on the tested Spark, and direct access to Metal private buffers remains unverified; the verified approach is to create the transferred tensors in compatible shared allocations from the outset.

**I plan to submit a pull request to oMLX to integrate MCDMA**, starting with the allocation and transfer support needed for this shared GPU-accessible memory path. That integration is planned and has not yet been implemented or submitted.

## Get started

### Hardware used

| Part | Tested setup |
|---|---|
| Mac | Mac Studio M3 Ultra, 256 GB, macOS 27 build `26A428` |
| Enclosure | OWC Mercury Helios 5S, connected by Thunderbolt 5 |
| NIC | Mellanox ConnectX-5 Ex MCX516A-CDAT, dual QSFP28, PCI `15b3:1019` |
| Peer | One DGX Spark using its ConnectX-7 Ethernet port |
| Current network link | 40GbE with the current cable; replacement pending |
| Replacement cables | 2 × Mellanox MCP1600-C001E30N, 100GbE QSFP28-to-QSFP28 passive copper DAC, 1 m; not yet validated in this setup |
| MTU | Ethernet 9000 bytes; headline benchmark RDMA path 1024 bytes, initial validation recipe 4096 bytes |

Use a separate management connection such as Wi-Fi or another Ethernet interface. MCDMA's `mcrdmaN` interfaces provide RDMA addressing, not ordinary TCP networking. The card's nominal port rate does not establish measured Thunderbolt throughput.

### Setup with an AI agent

Point your agent at [AGENTS.md](AGENTS.md), or paste this prompt:

> Help me set up MCDMA from https://github.com/ashhart/mcdma. Read AGENTS.md and the installation guide first, inspect my target Mac and Spark, clone and build the required files, and prepare the signed development candidate and link-configuration commands. Complete everything you can before asking me to act, then give me the exact remaining Recovery, approval or connection steps. After I confirm those are done, verify the loaded driver and real bidirectional RDMA before measuring latency. Keep my machine details and logs private, and do not claim untested GPU-memory access or throughput.

The agent can prepare the software and checks; Recovery security changes and macOS approval may still require you at the Mac. This workflow and the manual steps below use the same installation guide.

### 1. Clone and check prerequisites

Clone the repository:

```sh
git clone https://github.com/ashhart/mcdma.git MCDMA
cd MCDMA
sw_vers -buildVersion
xcode-select -p
xcrun --sdk macosx --show-sdk-version
```

You need physical access to the Mac, an administrator account, Python 3 and Xcode with the **macOS 27 SDK**. This installation recipe targets **26A428**; other macOS 27 builds are not automatically compatible. The source also recognizes the earlier inspected beta `26A5425a`, but it is not the basis of these measurements or this setup helper.

### 2. Enable RDMA and allow the development kext

Shut down, hold the power button for startup options, and enter Options → Continue. In Recovery, open Utilities → Startup Security Utility → select the startup disk → Security Policy, choose **Reduced Security**, and tick **Allow user management of kernel extensions from identified developers**.

Open Utilities → Terminal and run:

```sh
rdma_ctl enable
```

The current ad-hoc-signed development build also used this separate SIP exception in the lab:

```sh
csrutil disable
```

Restart into macOS. `rdma_ctl enable` enables Apple's RDMA feature; it does not install the CX5 driver. SIP disablement reduces system protection and is specific to this development setup, not a general requirement for built-in RDMA. Leave authenticated-root protection enabled. The [installation guide](docs/install.md) explains these settings, links Apple's documentation and shows how to undo them.

### 3. Build, install and approve

From the repository root:

```sh
python3 tools/build.py test
python3 tools/build.py native
```

The native build creates a disabled kernel extension, provider and validation clients. Follow [prepare the enabled, ad-hoc-signed copy](docs/install.md#2-build-and-prepare-a-local-copy), then run:

```sh
bash tools/install-native.sh --check
sudo /bin/bash tools/install-native.sh --install
```

The installer backs up an existing MCDMA installation and installs the kext, provider and provider configuration. Approve MCDMA in System Settings → Privacy & Security and restart when macOS requests it. Follow the guide's powered-off connection procedure; live removal with mapped DMA pages remains unvalidated.

### 4. Configure the link and launch checks

Follow [peer setup and address restoration](docs/install.md#4-configure-the-linux-peer-and-restore-the-mac-gid) to select the wired port, configure both static neighbors and build the Linux peer. Fill these variables with values observed on your hardware; private lab addresses are not built in:

```sh
python3 tools/restore-rdma.py --dry-run \
  --interface "$MAC_IF" --expected-mac "$MAC_HWADDR" \
  --peer-gid "$PEER_GID" --peer-mac "$PEER_HWADDR"

sudo python3 tools/restore-rdma.py \
  --interface "$MAC_IF" --expected-mac "$MAC_HWADDR" \
  --peer-gid "$PEER_GID" --peer-mac "$PEER_HWADDR"

rdma_ctl status
kmutil showloaded --list-only --variant-suffix release | grep org.mcdma.cx5.native
build/cx5-native-check --provider /usr/local/lib/rdma/libmcdma-rdmav34.so --require-gid
ibv_devinfo
```

Check the loaded version and UUID against your build, then require `PORT_ACTIVE` and a real RoCE v2 GID on the wired CX5 device, named `rdma_mcrdmaN`. Unused `rdma_enN` Thunderbolt ports or an unplugged second CX5 port can remain down. Restore the temporary address and neighbors after a reboot, checking interface enumeration again.

There is no separate driver daemon to launch: the approved kext attaches to the card at boot/connection, and verbs applications load the userspace provider. A green port alone is not a memory-transfer test.

### 5. Verify transfers and check your speeds

First run the [four-way byte-verifying test](docs/install.md#5-verify-rdma-before-using-it) in kernel mode, then direct mode and BlueFlame-64 mode. After all checks pass, build the optional helper on the Studio and run it in a separate Terminal to reproduce the GPU-active condition:

```sh
mkdir -p build
xcrun swiftc -O client/fabric_keepalive.swift -framework Metal -o build/fabric-keepalive
build/fabric-keepalive 0 small
```

`0` runs until you stop it with Ctrl-C; `30 small` runs for 30 seconds. No `sudo`, extension approval or restart is needed for the helper. See [keepalive usage and controls](docs/gpu-keepalive.md#build-and-run) before comparing results.

Use the same explicit SSH hosts, executable paths and live device names to record both initiation directions:

```sh
mkdir -p results
python3 tools/native_cross_host.py \
  --mac-host "$MAC_SSH" --peer-host "$PEER_SSH" \
  --mac-client "$MAC_CLIENT" --peer-client "$PEER_CLIENT" \
  --mac-provider /usr/local/lib/rdma/libmcdma-rdmav34.so \
  --mac-checker "$MAC_CHECKER" \
  --mac-interface "$MAC_IF" --peer-interface "$PEER_IF" \
  --mac-device "$MAC_RDMA_DEVICE" --peer-device "$PEER_RDMA_DEVICE" \
  --peer-gid-index "$PEER_GID_INDEX" --path-mtu 1024 \
  --payload-bytes 4096 --mac-cq-map 2 --mac-user-post 1 --mac-user-bf 64 \
  --mac-latency --peer-latency --output results/latency-4096-mtu1024-gpu-active-bf64.json

python3 tools/latency_summary.py results/latency-4096-mtu1024-gpu-active-bf64.mac-latency.csv
python3 tools/latency_summary.py results/latency-4096-mtu1024-gpu-active-bf64.spark-latency.csv
```

The [guide defines every variable](docs/install.md#5-verify-rdma-before-using-it). Use noninteractive SSH over the management network; both neighbors must already be configured. The summaries report median, p95, p99, maximum and sample counts. Also collect matching keepalive-off runs with fresh filenames, keeping the driver, provider, client binaries and MTU unchanged; the published 0.1.17 figures are not a guarantee for the 0.1.16 installer. Repeat at `--payload-bytes 1024` with a different output filename, and repeat each configuration before comparing it. Keep all logs in ignored `results/`; they can contain addresses and memory-region access keys.

**Throughput is a separate measurement.** This beta's supplied test measures 1 KiB/4 KiB latency at queue depth one; it does not yet provide a validated sustained-bandwidth test. Do not label payload divided by median latency as link throughput, or an `iperf3` TCP result as MCDMA RDMA bandwidth. Large-transfer and queue-depth throughput testing is planned alongside the latency work.

## First beta and what comes next

This is MCDMA's first public beta. Please watch this repository for updates: I will keep iterating, testing on real hardware and working to improve throughput and reduce latency. Once the driver is ready, I plan to pursue the appropriate Apple Developer ID signing and distribution requirements. The current development build is ad-hoc signed and is not notarized or approved for production use.

Today's recipe and measurements cover **one Spark and one Studio**. I will update the recipe for a **multi-ring setup with two Sparks and one Studio** as that topology is tested. Those multi-machine results are not established yet.

I also have around **25 experiments planned for heterogeneous AI workloads**: pipeline and tensor parallelism, prefill/decode handoff, moving KV-cache state, and models distributed across the machines' memory. The aim is to find where this connection improves real prompt processing and generation, beyond a microbenchmark.

## Scope and limits

The repository contains the native driver, userspace provider, build/setup tools and tests needed to validate CX5 RDMA. Runtime installation requires only the kernel extension, provider and provider configuration; the optional Metal keepalive runs separately. Historical benchmark archives, model-serving experiments, vendor firmware, SDK/KDK files, private installers and generated binaries are excluded.

The portable setup has offline checks, but a fresh-machine installation following this recipe has not yet been completed. Foreign-QPN isolation, process death with outstanding direct work and hot removal with mapped pages remain open. The shared GPU-accessible buffer checks described above do not establish direct access to existing CUDA device allocations or Metal private buffers, inference-engine integration, universal ConnectX support or zero-CPU operation. Read [removal and recovery](docs/install.md#removal-and-recovery) before installation.

See [architecture](docs/architecture.md), [hardware validation](docs/hardware-validation.md) and [provenance](PROVENANCE.md). MCDMA is licensed under [Apache 2.0](LICENSE).
