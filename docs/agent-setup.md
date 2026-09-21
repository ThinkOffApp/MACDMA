# MCDMA agent setup runbook

Read [AGENTS.md](../AGENTS.md), [the user guide](user-guide.md), [install.md](install.md) and [hardware validation](hardware-validation.md) before acting. This runbook organizes the existing installation tools; it is not a new installer or an inference connector.

## Operating rules

- Work on the user's designated Mac hosts and Sparks, not whichever machine runs the agent; a compatible MacBook Pro may be the endpoint rather than the Studio.
- Start with read-only discovery and one Spark. Preserve the existing inter-Spark and management links.
- Use a single recorded source revision across the installation. Preserve dirty checkouts and existing environments.
- Keep commands, results and progress in ignored `local/` and `results/`. Copy [the record template](examples/setup-record.md) into `local/` before filling it in.
- Do not store credentials in the record or include raw machine details in a public commit.
- Explain the documented development security changes before requesting owner action. Existing explicit authorization counts; do not repeatedly ask for it. Never infer approval from elapsed time.
- Do not use speculative firmware updates, hot unload, live cable removal, unrelated security changes or permissive SSH settings to work around failures.
- A failed checkpoint stays failed until evidence changes. Diagnose the actual error instead of rerunning privileged commands blindly.

For the user guide's MacBook layouts, verify each host's actual Thunderbolt capabilities and exact OS/SDK compatibility first. The published hardware results are Studio results, so do not report a MacBook configuration as validated until it passes. Two MacBook endpoints require separate enclosure/card installations and separate setup records; start with one Mac-to-Spark pair and preserve the inter-Spark link.

## A. Discover and prepare

1. Establish the target host identities, management access, desired number of links and requested scope: RDMA only, standalone engines too, or connector development as a separate task.
2. Record OS build, architecture, SDK, hardware, firmware, physical port mapping and existing software from the user guide's inventory commands.
3. Check the exact OS build against the current source and AGENTS.md. Stop installation on an unsupported build and report the mismatch; do not patch out the guard as setup.
4. Verify noninteractive SSH to both endpoints and enough free disk/memory for the requested build or model. Identify the Linux interface already serving the Spark pair.
5. Run the source tests and native build, then prepare and sign the separate enabled candidate exactly as in install.md. Stop if a candidate directory already exists until its identity is understood.
6. Build the Linux verbs peer from the same source revision using the documented dependencies. Save absolute executable paths on each endpoint.
7. Record candidate UUID, provider/client hashes and `tools/install-native.sh --check` result. Prepare the remaining commands without changing security policy automatically.

**Checkpoint A:** private inventory, matched builds and explicit remaining owner actions exist. No claim of working RDMA yet.

## B. Owner-assisted activation

Give the owner only the outstanding steps in install.md: Recovery policy, `rdma_ctl enable`, the current development SIP exception, normal boot, approval and requested restarts. Leave authenticated-root enabled. Explain that this is the current ad-hoc development recipe rather than a requirement of RDMA in general.

Run the installer after its prerequisites are satisfied, save the backup location and follow its actual errors. Use the documented shutdown/disconnection sequence. If remote access is lost, leave a resumable record rather than inventing a successful activation.

**Checkpoint B:** after boot, the expected kernel UUID is loaded, the matching provider is installed and the selected CX5 PCI function is owned by MCDMA. Record fresh interface/device identities.

## C. Configure one physical link

Read the current addressing section of install.md and use its commands. Fill all required values from discovery:

| Test variable | Required meaning |
|---|---|
| `MAC_SSH`, `PEER_SSH` | Management SSH destinations |
| `MAC_CLIENT`, `MAC_CHECKER`, `PEER_CLIENT` | Absolute executable paths on their respective hosts |
| `MAC_IF`, `PEER_IF` | Interfaces at the ends of this physical cable |
| `MAC_RDMA_DEVICE`, `PEER_RDMA_DEVICE` | Corresponding verbs devices |
| `MAC_HWADDR`, `PEER_HWADDR` | Observed hardware addresses |
| `MAC_GID`, `PEER_GID` | Correct MAC-derived link-local GIDs |
| `PEER_GID_INDEX` | Actual Linux RoCE v2 index for that GID and interface |

Apply Ethernet MTU 9000 to the selected link as documented, dry-run the Mac restore tool, then apply the configuration and reciprocal Linux neighbor. Do not change the interface belonging to the existing Spark pair. Confirm the selected RDMA path supports MTU 4096 before functional testing.

**Checkpoint C:** active devices, correct GIDs and both neighbors agree with the physical wiring. Ping is not a test of `mcrdmaN`.

## D. Verify RDMA

Run the full `native_cross_host.py` command from install.md with the values above. Save a unique output path for every attempt, including failed ones.

Require all four operations to succeed with verified bytes in these arms, in order:

1. Kernel posting: CQ map 0, userspace post 0, BlueFlame 0.
2. Direct posting: CQ map 2, userspace post 1, BlueFlame 0.
3. BlueFlame-64: CQ map 2, userspace post 1, BlueFlame 64.

Inspect requested-mode confirmation as well as transfer results. Do not reinterpret a fallback to kernel posting as a passing userspace test. Record both initiators and both verbs independently.

**Checkpoint D:** verified host-memory RDMA on one link. This does not establish GPU-buffer latency or inference speed.

## E. Expand and measure

Repeat C and D for the second Spark after the documented powered-down cabling sequence. Keep the first link's configuration intact. Then use the bounded concurrent methodology in [the validation report](validation-2026-09-17.md), recording both per-link rates and aggregate behavior.

Measure performance only after correctness passes. Record payload, queue depth, posting mode, MTUs, link speed, repetitions, warmups, power/keepalive state, initiation direction and measurement boundary. Preserve raw samples; do not convert an operation's completion latency into a one-way wire latency claim.

Do not start a persistent Metal keepalive or install configuration services just because they exist. Explain their effects and use them when included in the user's chosen setup. Consult [CLI documentation](../cli/README.md) for persistence and verify again after a restart.

**Checkpoint E:** a reproducible transport result for the installed build, with limits stated.

## F. Optional standalone inference

Read the engine section of the user guide. Discover existing environments before installing anything. Obtain the user's model choice or agree on a small baseline before downloading large models.

For vLLM, use NVIDIA's current DGX Spark recipe, pin a compatible image digest/model revision and confirm the container uses the GPU. For oMLX, record the selected repository and revision, isolate dependencies and confirm a small model runs on the Studio. Bind API ports to loopback for initial testing and use management SSH tunnels where needed.

Capture a short deterministic request, engine logs, model/version identity and an actual completed response from each engine. Record baseline prompt length, time to first token and decode rate using one consistent client if performance is requested.

**Checkpoint F:** two working standalone engines, not an MCDMA inference cluster.

A requested vLLM-to-oMLX handoff must stop at the missing integration boundary unless the user supplies a connector or authorizes its implementation as further work. The historical file-staged experiment is described in the repository, but its scripts are not shipped here. Do not invent flags, a KV connector name, support for arbitrary `cudaMalloc`, or a working heterogeneous MCDMA collective backend.

## Completion report and resume behavior

Report the last passed checkpoint, exact build identity, per-link READ/WRITE outcomes, any engine baselines and outstanding work. Distinguish owner actions from missing code and hardware failures. Include the private record path and rollback reference.

When resuming after a reboot or interruption, read the record and recheck live host/build/interface identity. Reuse successful evidence only where the relevant hardware and configuration have not changed. Never infer that an owner approval loaded the correct build without checking it.
