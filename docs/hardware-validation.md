# Hardware validation

The default build does not enable PCI ownership. The [developer installation guide](install.md) covers setup and recovery; its portable procedure still needs a fresh-machine end-to-end check, and the original machine-specific installers are outside this tree.

For an already approved lab installation, record the exact kernel UUID, OS build, SDK, provider/client hashes, NIC identity, firmware, physical link, Ethernet MTU and RC path MTU. Resolve each interface from its actual hardware identity after every restart rather than assuming its previous enumeration.

Require native discovery with a real RoCE v2 GID, mapping-protection checks, and four-way byte verification before any timing. The `client/` programs exercise those checks; the cross-host runner requires explicit hosts, executable paths, interfaces and devices. Its `--help` output documents the arguments without supplying private lab defaults.

Begin with kernel posting and ordinary direct posting, then test each BlueFlame arm independently. Missing mode markers or a failed transfer invalidate the arm. Time only accepted configurations with identical clients, payloads, queue depth, warmups and completion boundaries, alternating order and retaining every sample.

Raw runner output contains hostnames, paths, addresses, keys used for the test's registered regions and device details. Store it in ignored `results/` or outside this repository, and review it before sharing. Historical benchmark archives are retained outside this driver repository.

A timing improvement alone does not prove the final pmap cache attribute, one-way wire latency, GPU memory access or a hardware latency floor.
