"""The listen end of one mcdma-rpcd link: map its mailbox, register as its service, answer requests."""

from __future__ import annotations

import ctypes
import mmap
import os
import select
import socket
from collections.abc import Iterable
from contextlib import suppress
from pathlib import Path
from typing import Any

# Protocol 1 layout; docs/link-daemon.md describes it.
CTRL = 4096
REQUEST_WORD = 0
STAGED_WORD = 128
SIZES = 256
DEFAULT_HALF = 4 << 20
ABI = 1
_SEQ = 0xFFFFFFFF
_LIBRARIES = (
    "/usr/local/lib/libmcdma-rpc.so",
    "/usr/lib/libmcdma-rpc.so",
    "/usr/local/lib/libmcdma-rpc.dylib",
)


class MailboxError(RuntimeError):
    """The mailbox could not be attached or used."""


def load_helper(path: str | None = None) -> Any:
    """libmcdma-rpc, which provides the mailbox words' acquire and release ordering."""
    candidates = [path] if path else [os.environ.get("MCDMA_RPC_LIBRARY", ""), *_LIBRARIES]
    for candidate in candidates:
        if not candidate or not Path(candidate).is_file():
            continue
        library = ctypes.CDLL(candidate)
        library.mcdma_rpc_abi.restype = ctypes.c_uint32
        if library.mcdma_rpc_abi() != ABI:
            raise MailboxError(f"{candidate} speaks ABI {library.mcdma_rpc_abi()}, not {ABI}")
        library.mcdma_rpc_wait_word.restype = ctypes.c_uint64
        library.mcdma_rpc_wait_word.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_int,
            ctypes.c_uint64,
            ctypes.c_uint64,
        ]
        library.mcdma_rpc_store_word.restype = None
        library.mcdma_rpc_store_word.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        return library
    raise MailboxError("libmcdma-rpc is not installed; set MCDMA_RPC_LIBRARY")


def _read_line(control: socket.socket, limit: int = 256) -> bytes:
    received = b""
    while not received.endswith(b"\n") and len(received) < limit:
        chunk = control.recv(1)
        if not chunk:
            break
        received += chunk
    return received.strip()


class ServiceMailbox:
    """A registered service on one link; requests land in the request half, replies go out from the reply half."""

    def __init__(self, name: str, *, socket_path: str | None = None, mailbox_path: str | None = None,
                 helper: Any = None) -> None:
        self.name = name
        self._helper = helper or load_helper()
        directory = os.environ.get("MCDMA_RPC_BOX_DIR", "/dev/shm")
        path = mailbox_path or f"{directory}/mcdma-rpc.{name}"
        try:
            descriptor = os.open(path, os.O_RDWR)
        except OSError as exc:
            raise MailboxError(f"cannot open mailbox {path}: {exc}") from exc
        try:
            self._map = mmap.mmap(descriptor, os.fstat(descriptor).st_size)
        finally:
            os.close(descriptor)
        self.buffer = memoryview(self._map)
        self._base = ctypes.addressof(ctypes.c_char.from_buffer(self._map))
        self.request_bytes = self._load(SIZES) or DEFAULT_HALF
        self.reply_bytes = self._load(SIZES + 8) or DEFAULT_HALF
        if self.request_bytes + self.reply_bytes > len(self.buffer):
            self.close()
            raise MailboxError(f"mailbox {path} is smaller than the halves it announces")
        self._control = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        control_path = socket_path or f"/tmp/mcdma-rpcd.{name}.sock"
        try:
            self._control.settimeout(5.0)
            self._control.connect(control_path)
            self._control.sendall(b"MODE poll\n")
            answer = _read_line(self._control)
            self._control.settimeout(None)
        except OSError as exc:
            self.close()
            raise MailboxError(f"cannot register with mcdma-rpcd at {control_path}: {exc}") from exc
        if answer != b"OK":
            self.close()
            raise MailboxError(f"mcdma-rpcd at {control_path} refused the service: {answer.decode(errors='replace')}")
        # A request left from an earlier service is not for this one.
        self._last = self._load(REQUEST_WORD) >> 32

    def _load(self, offset: int) -> int:
        return int.from_bytes(self.buffer[offset:offset + 8], "little")

    @property
    def max_reply(self) -> int:
        return self.reply_bytes - CTRL

    @property
    def alive(self) -> bool:
        """Whether the daemon still holds the registration; it sends nothing until it ends it."""
        try:
            readable, _, _ = select.select([self._control], [], [], 0)
        except (OSError, ValueError):
            return False
        return not readable

    def next_request(self, timeout_s: float) -> tuple[int, memoryview] | None:
        """The next landed request as (sequence, payload), or None after `timeout_s`."""
        word = self._helper.mcdma_rpc_wait_word(self._base + REQUEST_WORD, self._last, 0, 100_000,
                                                int(timeout_s * 1e9))
        if not word:
            return None
        length = word & _SEQ
        if length > self.request_bytes - CTRL:
            raise MailboxError("request length exceeds the request half")
        self._last = word >> 32
        return self._last, self.buffer[CTRL:CTRL + length]

    def reply_area(self) -> memoryview:
        """The reply payload area, for writing a reply in place before `publish`."""
        start = self.request_bytes + CTRL
        return self.buffer[start:self.request_bytes + self.reply_bytes]

    def publish(self, seq: int, length: int) -> None:
        """Hand the `length` bytes now in the reply area to the daemon as the answer to `seq`."""
        if length > self.max_reply:
            raise MailboxError("reply is larger than the reply half")
        self._helper.mcdma_rpc_store_word(self._base + self.request_bytes + STAGED_WORD,
                                          (seq & _SEQ) << 32 | length)

    def reply(self, seq: int, parts: Iterable[Any]) -> None:
        """Copy `parts` into the reply area and publish them as the answer to `seq`."""
        area, offset = self.reply_area(), 0
        for part in parts:
            view = memoryview(part).cast("B")
            if offset + view.nbytes > len(area):
                raise MailboxError("reply is larger than the reply half")
            area[offset:offset + view.nbytes] = view
            offset += view.nbytes
        self.publish(seq, offset)

    def close(self) -> None:
        with suppress(AttributeError, OSError):
            self._control.close()
        with suppress(AttributeError, BufferError):
            self.buffer.release()
        with suppress(AttributeError, BufferError):
            self._map.close()
