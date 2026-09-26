# Real-payload correctness check

`run_bw.py --payload /source/file --dump /new/destination` transfers one
resident file through the registered buffers. Select one `--ops write` or
`--ops read`, one size, one depth, `--qps 1`, one initiator, one CQ mode,
`--repeats 1` and `--warmup 0`. Both clients must be updated. Paths are on
the respective data-source and receiver hosts; for READ the responder is
the source and the initiator receives the dump.

The nonempty regular file must fit `bytes * agreed_depth` after resource
clamping; oversized inputs fail instead of wrapping or truncating. The file
length replaces `--total`. The final request is zero-padded and the dump
excludes padding. Each slot is transferred once; SEND and multi-QP file
layouts are rejected. This is not an arbitrary-size streaming handoff.

The receiver checks the entire file with CRC-64/ECMA and validates DMA
guards before exclusively creating the destination; existing files and
symlinks are not overwritten. CRC64 detects accidental corruption, not
malicious modification. A checksum failure counts as one mismatch and
prevents a dump. File I/O and checksums are outside the RDMA timer; rows
are labelled `measurement=resident-payload` and rejected by the sustained
bandwidth summariser. Use seeded mode for sustained-bandwidth comparisons.

# Two Linux hosts

`--mac-platform linux` lets `run_bw.py` drive two Linux RDMA hosts on stock
rdma-core. The `--mac-*` flags then name the first Linux host. Compile
`mcdma_bw.c` on both (`gcc -O2 mcdma_bw.c -libverbs -o mcdma-bw`) and run:

```sh
python3 benchmarks/run_bw.py --mac-platform linux \
  --mac-host "$A_SSH" --peer-host "$B_SSH" \
  --mac-bw "$A_BW" --peer-bw "$B_BW" \
  --mac-interface "$A_IF" --peer-interface "$B_IF" \
  --mac-device "$A_RDMA_DEVICE" --peer-device "$B_RDMA_DEVICE" \
  --mac-gid-index "$A_GID_INDEX" --peer-gid-index "$B_GID_INDEX" \
  --ops write --sizes 4194304 --depths 1 --qps 1 --total 8589934592 \
  --mtu 4096 --finish flag --verify-bytes 1048576 --timeout 120 \
  --output results/linux-pair --dry-run
```

Drop `--dry-run` to run it. `--mac-provider`, `--mac-checker` and the
`--mac-cq-map`, `--mac-user-post` and `--mac-user-bf` modes do not apply and
are rejected. The command runs with no `env IBV_DRIVERS` or `MCDMA_*` prefix,
and `--mac-gid-index` defaults to 1. Preflight gives both hosts the same
check: the GID index must be RoCE v2 on the named interface, binaries are
hashed with `sha256sum`, and each host needs the other's static IPv6
neighbour (`ip -6 neigh`). The manifest records `mac_platform`, and a
`MCDMA_*` provider marker on the first host fails the run.
