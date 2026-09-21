# Source follow-ups after 0.1.18

These changes build on 0.1.18 without changing its published latency or
throughput claims. The installed driver and release packages are not replaced
by merging source changes.

- PR #1: compare complete MAC octets, IPv6 address, interface and permanent
  neighbour status, accepting macOS's omitted leading zeros without matching
  a different address that happens to share a suffix.
- PR #2: add a bounded resident-payload check with correct READ direction,
  complete CRC64 verification, exact output length and exclusive destination
  creation. Oversized files fail; this is not arbitrary-size streaming or a
  sustained-bandwidth result. See [usage](../benchmarks/README.md).
- PR #3: accept ConnectX-4 Lx PF while retaining its actual device identity
  through the provider, checker and CLI. Apple Thunderbolt peer support is
  deferred for separate hardware validation; see [hardware scope](hardware-validation.md#additional-card-and-peer-support).

The original contributions are by [Chad Hurley](https://github.com/chadhurley25075-png),
with maintainer corrections and regression coverage. Tests exercise neighbour
matching, payload direction, slot capacity, partial final requests, corruption,
existing-file protection, device identity and rejected unsupported modes.
The complete initiator/responder trial paths also run against a memory-copy
verbs test double, including checksum, guard and completion failures.
Offline checks do not validate new hardware or establish a performance gain.
