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
