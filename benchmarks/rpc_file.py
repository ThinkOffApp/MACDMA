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


def pull(client, fname, dest):
    start = client.req + CTRL
    n = _request(client, STAT, 0, 0, fname)
    if _is_error(client, n) or n != 8:
        raise SystemExit(f'{fname}: not served')
    size = struct.unpack_from('<Q', client.buf, start)[0]
    frame = client.rep - CTRL
    t0 = time.perf_counter()
    done = 0
    with open(dest, 'wb', buffering=0) as out:
        while done < size:
            n = _request(client, READ, done, min(frame, size - done), fname)
            if _is_error(client, n) or n == 0:
                raise SystemExit(f'{fname}: read failed at {done}')
            out.write(client.buf[start:start + n])
            done += n
    secs = time.perf_counter() - t0
    return {'bytes': size, 'seconds': secs, 'gbit': size * 8 / secs / 1e9 if secs else None,
            'frame_bytes': frame}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='mode', required=True)
    s = sub.add_parser('serve')
    s.add_argument('name')
    s.add_argument('directory')
    c = sub.add_parser('pull')
    c.add_argument('name')
    c.add_argument('file', help='plain name inside the served directory')
    c.add_argument('dest')
    args = p.parse_args(argv)
    if args.mode == 'serve':
        print(json.dumps({'served': serve(args.name, args.directory)}))
        return 0
    client = Client(args.name)
    try:
        if not client.up():
            raise SystemExit('link is down')
        print(json.dumps(pull(client, args.file, args.dest)))
    finally:
        client.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
