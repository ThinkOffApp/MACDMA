#!/usr/bin/env python3
"""Pull a file across one mcdma-rpcd link.

`serve NAME DIR` runs on the listen host next to the file's owner (for a
KV handoff: the producer's llama-server slot directory). It answers two
requests: STAT (file size) and READ (bytes at an offset, straight from the
file into the reply half with readinto). Only plain names inside DIR are
served. `pull NAME FILE DEST` runs on the connect host: it pulls the file in
reply-sized frames straight from the mailbox into DEST and prints one JSON
line with bytes, seconds and Gbit/s. The link stays up between pulls, so
there is no per-transfer setup (unlike mcdma-bw's resident payload runs).
"""
import argparse
import json
import os
from pathlib import Path
import struct
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rpc_roundtrip import CTRL, Client, ServiceMailbox  # noqa: E402

REQ = struct.Struct('<BQI')      # op, offset, length; the file name follows
STAT, READ = 1, 2
ERR = 0xFFFFFFFF                 # reply length on error is 4: this marker


def _name_ok(name):
    return name and '/' not in name and name not in ('.', '..') and not name.startswith('.')


def handle(root, payload, area, max_reply):
    """Answer one request into `area`; returns the reply length."""
    try:
        op, offset, length = REQ.unpack_from(payload)
        fname = bytes(payload[REQ.size:]).decode()
        if not _name_ok(fname):
            raise ValueError('bad name')
        path = root / fname
        if op == STAT:
            area[:8] = struct.pack('<Q', path.stat().st_size)
            return 8
        if op == READ:
            length = min(length, max_reply)
            with open(path, 'rb', buffering=0) as f:
                f.seek(offset)
                return f.readinto(area[:length]) or 0
        raise ValueError('bad op')
    except (OSError, ValueError, struct.error, UnicodeDecodeError):
        area[:4] = struct.pack('<I', ERR)
        return 4


def serve(name, directory, helper=None, socket_path=None, mailbox_path=None, stop_after=None):
    box = ServiceMailbox(name, socket_path=socket_path, mailbox_path=mailbox_path, helper=helper)
    root = Path(directory).resolve()
    served = 0
    try:
        while box.alive and (stop_after is None or served < stop_after):
            got = box.next_request(1.0)
            if not got:
                continue
            seq, payload = got
            box.publish(seq, handle(root, payload, box.reply_area(), box.max_reply))
            served += 1
    finally:
        box.close()
    return served


def _request(client, op, offset, length, fname, timeout_s=10.0):
    """One call with a raw payload; returns the reply length (bytes are in the reply half)."""
    head = REQ.pack(op, offset, length) + fname.encode()
    gen = client._load(72)
    client.seq = (client.seq % 0xFFFFFFFF) + 1
    client.buf[CTRL:CTRL + len(head)] = head
    client.helper.mcdma_rpc_store_word(client.base, client.seq << 32 | len(head))
    deadline = time.perf_counter() + timeout_s
    while True:
        word = client.helper.mcdma_rpc_wait_word(client.base + client.req + 64, (client.seq - 1) & 0xFFFFFFFF,
                                                 0, 100_000, 50_000_000)
        if word and (word >> 32) == client.seq:
            return word & 0xFFFFFFFF
        if client._load(72) != gen:
            raise RuntimeError('link generation changed during the call')
        if time.perf_counter() > deadline:
            raise TimeoutError('no reply')


def _is_error(client, n):
    start = client.req + CTRL
    return n == 4 and struct.unpack_from('<I', client.buf, start)[0] == ERR


def _stat(client, fname):
    n = _request(client, STAT, 0, 0, fname)
    if _is_error(client, n) or n != 8:
        raise SystemExit(f'{fname}: not served')
    return struct.unpack_from('<Q', client.buf, client.req + CTRL)[0]


def _pull_range(client, fname, fd, first, end, errors):
    start = client.req + CTRL
    frame = client.rep - CTRL
    done = first
    try:
        while done < end:
            n = _request(client, READ, done, min(frame, end - done), fname)
            if _is_error(client, n) or n == 0:
                raise RuntimeError(f'{fname}: read failed at {done}')
            os.pwrite(fd, client.buf[start:start + n], done)
            done += n
    except Exception as exc:   # reported by the caller; a thread must not die silently
        errors.append(exc)


def pull(clients, fname, dest):
    """Pull `fname` into `dest`. With several links (one Client each, each
    with its own rpc_file.py service) the file is split into contiguous
    ranges pulled concurrently, so one link's file copies overlap another's
    wire time. Protocol 1 allows one call at a time per link, so a single
    link runs read, wire and write strictly in series (measured 26 Sep:
    12 Gbit/s on a link that echoes at 26)."""
    if not isinstance(clients, (list, tuple)):
        clients = [clients]
    size = _stat(clients[0], fname)
    frame = min(c.rep for c in clients) - CTRL
    per = -(-size // len(clients))
    per = -(-per // frame) * frame or frame      # whole frames per link
    ranges = [(i * per, min(size, (i + 1) * per)) for i in range(len(clients))]
    errors = []
    fd = os.open(dest, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    try:
        os.ftruncate(fd, size)
        t0 = time.perf_counter()
        threads = [threading.Thread(target=_pull_range, args=(c, fname, fd, a, b, errors))
                   for c, (a, b) in zip(clients, ranges) if a < b]
        for th in threads:
            th.start()
        for th in threads:
            th.join()
        secs = time.perf_counter() - t0
    finally:
        os.close(fd)
    if errors:
        raise SystemExit(str(errors[0]))
    return {'bytes': size, 'seconds': secs, 'gbit': size * 8 / secs / 1e9 if secs else None,
            'frame_bytes': frame, 'links': len(clients)}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='mode', required=True)
    s = sub.add_parser('serve')
    s.add_argument('name')
    s.add_argument('directory')
    c = sub.add_parser('pull')
    c.add_argument('name', help='link name, or several comma-separated to pull ranges concurrently')
    c.add_argument('file', help='plain name inside the served directory')
    c.add_argument('dest')
    args = p.parse_args(argv)
    if args.mode == 'serve':
        print(json.dumps({'served': serve(args.name, args.directory)}))
        return 0
    clients = [Client(n) for n in args.name.split(',')]
    try:
        down = [n for n, c in zip(args.name.split(','), clients) if not c.up()]
        if down:
            raise SystemExit(f'link down: {down}')
        print(json.dumps(pull(clients, args.file, args.dest)))
    finally:
        for c in clients:
            c.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
