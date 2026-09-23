"""What a handoff exports: each layer's page rows, what their dimensions hold, and how frames split them."""

from __future__ import annotations

import time
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any

from . import wire

# Element types the decoder accepts, by the name the manifest uses.
DTYPES = ("bfloat16", "float16", "float32")


Probe = Callable[[int, int, int, int], tuple[int, ...]]


def _by_probe(shape: tuple[int, ...], args: list[int], mla: bool, probe: Probe) -> list[str] | None:
    """Labels from the backend's own shape function: bumping one argument shows which dimension it owns."""
    if tuple(probe(*args)) != shape:
        return None
    labels: list[str | None] = [None] * len(shape)
    owners = [("block", 0), ("token", 1), ("head_dim", 3)] + ([] if mla else [("head", 2)])
    for name, argument in owners:
        bumped = list(args)
        bumped[argument] += 16
        changed = [index for index, (a, b) in enumerate(zip(shape, probe(*bumped))) if a != b]
        if len(changed) != 1 or labels[changed[0]] is not None:
            return None
        labels[changed[0]] = name
    for index, size in enumerate(shape):
        if labels[index] == "head_dim" and mla:
            labels[index] = "latent"
        elif labels[index] == "head_dim" and size == 2 * args[3]:
            labels[index] = "kv_head_dim"
        elif labels[index] is None and size == 2 and not mla:
            labels[index] = "kv"
    return None if None in labels else [str(label) for label in labels]


def _by_size(shape: tuple[int, ...], args: list[int], mla: bool) -> list[str] | None:
    """Labels from sizes alone; None when two dimensions could hold the same thing."""
    rows, tokens, heads, head_size = args
    wanted = {"block": rows, "token": tokens}
    wanted.update({"latent": head_size} if mla else {"head": heads, "head_dim": head_size})
    labels: list[str] = []
    for size in shape:
        names = [name for name, value in wanted.items() if value == size and name not in labels]
        if not mla and size == 2 * head_size and not {"head_dim", "kv_head_dim"} & set(labels):
            names.append("kv_head_dim")
        if not mla and size == 2 and "kv" not in labels:
            names.append("kv")
        if len(names) != 1:
            return None
        labels.append(names[0])
    return labels


def geometry(shape: tuple[int, ...], *, num_blocks: int, block_size: int, heads: int, head_size: int,
             mla: bool, probe: Probe | None = None) -> tuple[list[str], int]:
    """Each dimension's label, and how many tensor rows hold one of the scheduler's blocks."""
    ratio = 1
    while block_size % ratio == 0 and ratio <= block_size:
        args = [num_blocks * ratio, block_size // ratio, heads, head_size]
        if args[0] in shape:
            labels = None
            if probe is not None:
                try:
                    labels = _by_probe(tuple(shape), args, mla, probe)
                except Exception:
                    labels = None
            labels = labels or _by_size(tuple(shape), args, mla)
            if labels:
                return labels, ratio
        ratio *= 2
    raise ValueError(f"cannot tell what the dimensions of cache shape {tuple(shape)} hold")


def plan_frames(layers: list[tuple[int, int, int]], capacity: int) -> list[tuple[int, int, int]]:
    """(layer, row_start, rows) frames that each fit `capacity` bytes, from (layer, rows, row_bytes) entries."""
    frames = []
    for index, rows, row_bytes in layers:
        per_frame = capacity // row_bytes
        if per_frame < 1:
            raise ValueError(f"one page row of layer {index} is larger than the reply half")
        frames.extend((index, start, min(per_frame, rows - start)) for start in range(0, rows, per_frame))
    return frames


@dataclass
class LayerPages:
    """One layer's exported rows of one cache tensor."""

    index: int
    tensor: Any
    block_dim: int
    rows: list[int]
    dims: list[str]
    dtype: str
    heads: int = 0
    total_heads: int = 0
    head_size: int = 0
    latent_size: int = 0
    rope_size: int = 0

    @property
    def shape(self) -> list[int]:
        return [len(self.rows) if dim == "block" else int(size)
                for dim, size in zip(self.dims, self.tensor.shape)]

    @property
    def row_bytes(self) -> int:
        size = 1
        for dim, value in zip(self.dims, self.shape):
            size *= 1 if dim == "block" else value
        return size * int(self.tensor.element_size())

    def to_dict(self) -> dict[str, Any]:
        kind = "mla" if "latent" in self.dims else "attention"
        entry = {"index": self.index, "kind": kind, "shape": self.shape, "dims": self.dims, "dtype": self.dtype}
        if kind == "mla":
            entry.update(latent_size=self.latent_size, rope_size=self.rope_size)
        else:
            entry.update(heads=self.heads, total_heads=self.total_heads, head_size=self.head_size)
        return entry


@dataclass
class Export:
    """One request's pages, held until the decoder closes the handoff or it expires."""

    request_id: str
    handoff: bytes
    tokens: list[int]
    first_token: int
    tokens_per_row: int
    layers: list[LayerPages]
    frames: list[tuple[int, int, int]] = field(default_factory=list)
    ready: Any = None
    created: float = field(default_factory=time.monotonic)

    def manifest(self, *, model: str, tp_rank: int, tp_size: int) -> dict[str, Any]:
        return {
            "protocol": wire.VERSION,
            "handoff": self.handoff.hex(),
            "model": model,
            "prompt_tokens": len(self.tokens),
            "first_token": self.first_token,
            "token_sha256": wire.token_sha256(self.tokens),
            "block_size": self.tokens_per_row,
            "tp_rank": tp_rank,
            "tp_size": tp_size,
            "layers": [layer.to_dict() for layer in self.layers],
            "frames": len(self.frames),
        }
