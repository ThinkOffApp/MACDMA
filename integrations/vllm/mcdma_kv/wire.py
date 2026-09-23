"""KV handoff protocol 1 message headers; docs/kv-handoff.md is the specification."""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass

MAGIC = b"MKVH"
VERSION = 1
HEADER_BYTES = 128
# Requests the decoder sends, and the answers a producer gives.
OPEN, MANIFEST, WAIT, ERROR, PULL, DATA, CLOSE, ACK = range(1, 9)
_KINDS = frozenset(range(1, 9))
# Kinds whose payload follows the header.
_CARRIERS = frozenset({OPEN, MANIFEST, ERROR, DATA})
# Header flag: `crc` holds the zlib CRC-32 of the payload.
CHECKED = 1
# magic, version, kind, handoff, frame, frames, layer, flags, row_start, rows, nbytes, crc
_LAYOUT = struct.Struct("<4sHH16sIIIIQQQI")


class WireError(ValueError):
    """A handoff message is malformed."""


@dataclass(frozen=True)
class Header:
    """One message header; DATA headers also say where the frame's pages belong."""

    kind: int
    handoff: bytes
    frame: int = 0
    frames: int = 0
    layer: int = 0
    flags: int = 0
    row_start: int = 0
    rows: int = 0
    nbytes: int = 0
    crc: int = 0


def pack(header: Header) -> bytes:
    """Serialize a header into exactly HEADER_BYTES bytes."""
    if header.kind not in _KINDS or len(header.handoff) != 16:
        raise WireError("a handoff header needs a known kind and a 16-byte id")
    packed = _LAYOUT.pack(
        MAGIC,
        VERSION,
        header.kind,
        header.handoff,
        header.frame,
        header.frames,
        header.layer,
        header.flags,
        header.row_start,
        header.rows,
        header.nbytes,
        header.crc,
    )
    return packed.ljust(HEADER_BYTES, b"\0")


def unpack(payload: memoryview | bytes) -> Header:
    """Parse the header at the start of `payload`, or raise WireError."""
    if len(payload) < HEADER_BYTES:
        raise WireError("message is shorter than a handoff header")
    magic, version, kind, *fields = _LAYOUT.unpack_from(payload, 0)
    if magic != MAGIC or version != VERSION:
        raise WireError("message is not a KV handoff protocol 1 header")
    if kind not in _KINDS:
        raise WireError(f"unknown handoff message kind {kind}")
    header = Header(kind, *fields)
    if kind in _CARRIERS and len(payload) < HEADER_BYTES + header.nbytes:
        raise WireError("handoff payload is shorter than its header says")
    return header


def body(payload: memoryview | bytes, header: Header) -> memoryview:
    """The payload that follows `header`."""
    return memoryview(payload)[HEADER_BYTES : HEADER_BYTES + header.nbytes]


def token_sha256(tokens: list[int]) -> str:
    """Digest of prompt token IDs as little-endian uint32 values, compared by both ends."""
    return hashlib.sha256(b"".join(int(token).to_bytes(4, "little") for token in tokens)).hexdigest()
