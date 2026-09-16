# 0.1.18 development release

Version 0.1.18 expands memory registration and verbs support and fixes resource ownership during teardown. It ships with CLI 1.1.0. No matched latency or sustained-throughput improvement over 0.1.17 has been established.

## Driver changes

- Register larger buffers by walking the IOMMU mapping and selecting the largest usable MKey page size, rather than relying on the earlier small-page capacity. Hardware acceptance registered a 4 MiB payload plus 16 KiB of guard space; maximum-size and heavily fragmented mappings remain untested.
- Reclaim resources belonging to a closed context and hold the context alive while reclaiming its children. Regression tests cover the use-after-free found when the last child disappeared during `dealloc_context`.
- Increase object capacity to 256 per device and 64 per context, with explicit mapping quotas and ownership checks.
- Add scatter/gather, inline posting, immediate-data operations and send flags in the provider. These additions have offline tests; their full hardware matrix has not been validated. Queue capacities remain bounded, and this is not a general-purpose verbs provider.
- Add a lifecycle runner and a queue-depth bandwidth runner, with raw logs, mode checks, byte verification and guard checks. Correct the bandwidth guard checker so it does not mistake the final 64 payload bytes for guard corruption; sweeps now stop after the first failed pair.

## CLI changes

The CLI under [`cli/`](../cli/README.md) runs with Node.js 20 or newer and has no npm runtime dependencies.

- Compare numeric versions and reject package downgrades before requesting administrator access, then recheck the installed version after authentication.
- Verify package checksums, bounded archive contents, kext identity and signature before replacing installed files. Locally generated packages are not committed; the package builder requires an installed 0.1.18 driver.
- Use private temporary directories for administrator scripts and SSH multiplexing; reject SSH aliases that can be interpreted as options.
- Invalidate old transfer results when the loaded driver UUID, provider hash, link identity or test configuration changes.
- Explicitly select kernel, direct or BlueFlame-64 posting and require the provider's matching mode markers before reporting a pass.
- Retrieve and validate all 2,000 raw samples from each initiator during a latency test. Peer binaries use 1,000 samples per operation; unsupported `test.iterations` values now fail instead of being silently ignored.
- Add `mcdma bandwidth LINK --output DIR`, which invokes the repository's sustained-bandwidth runner and keeps its CSVs, hashes and logs. It requires the source checkout, Python 3 and compiled bandwidth tools on both endpoints.

## Hardware acceptance

On macOS 27 build `26A428`, using a Mac Studio M3 Ultra, Helios 5S and ConnectX-5 Ex:

| Check | Observed result |
|---|---|
| 4 KiB WRITE/READ, both initiators, kernel/direct/BlueFlame-64 | 12 operations passed byte verification |
| 4 MiB WRITE/READ, both initiators, kernel posting | Four transfers passed full payload verification, guard checks and cleanup |
| CQ and user queue mappings | Protection, ownership and retirement checks passed |
| Bounded lifecycle checks across three posting modes | 21 cases passed, including 12 deliberate client terminations, with successful fresh transfers after each case |
| CLI quick check with two Sparks | Both Mac-facing 100 Gb/s links passed bidirectional byte verification |
| CLI latency trace collection | 2,000 validated samples from each initiator, with posting mode confirmed |
| CLI bandwidth smoke check | Four 1 MiB single-transfer cases passed sampled payload checks, guards and cleanup |

These checks establish the tested cases only. Killing a client immediately after posting does not establish that a request was still in flight at termination. Long process-death stress, concurrent clients, active foreign-QPN doorbells and mapped hot removal remain outstanding. The original unattributed panic is not proven fixed.

The README's latency figures remain the historical 0.1.17 measurements with their original conditions. The single-transfer checks above are not sustained bandwidth measurements. The portable installation and package-verification changes have offline checks; a fresh-machine installation using the revised public tools has not been repeated. This release does not change the existing limitations for `cudaMalloc` registrations or Metal private buffers.
