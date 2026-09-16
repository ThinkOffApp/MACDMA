#!/usr/bin/env python3
"""Verify and extract a bounded MCDMA package without following archive links."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import plistlib
import re
import tarfile


def extract(archive, destination, manifest):
    data = Path(archive).read_bytes()
    if len(data) > 64 * 1024 * 1024 or hashlib.sha256(data).hexdigest() != manifest.get('archive_sha256'):
        raise ValueError('Archive checksum or size mismatch')
    expected = manifest.get('files')
    if not isinstance(expected, dict) or not expected or len(expected) > 64:
        raise ValueError('A complete package file manifest is required')
    destination = Path(destination)
    destination.mkdir(mode=0o700)
    seen = set()
    total = 0
    with tarfile.open(archive, 'r:gz') as tar:
        for index, member in enumerate(tar):
            if index >= 128:
                raise ValueError('Too many archive members')
            name = PurePosixPath(member.name)
            if name.is_absolute() or '..' in name.parts or '\\' in member.name:
                raise ValueError('Unsafe archive path')
            if member.isdir():
                continue
            key = str(name)
            if not member.isfile() or key not in expected or key in seen or member.size > 32 * 1024 * 1024:
                raise ValueError('Unexpected archive member')
            total += member.size
            if total > 64 * 1024 * 1024:
                raise ValueError('Archive exceeds extracted size limit')
            digest = expected[key]
            if not isinstance(digest, str) or not re.fullmatch('[0-9a-f]{64}', digest):
                raise ValueError('Invalid file checksum')
            contents = tar.extractfile(member).read()
            if hashlib.sha256(contents).hexdigest() != digest:
                raise ValueError('Package member checksum mismatch: ' + key)
            target = destination.joinpath(*name.parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(contents)
            target.chmod(0o755 if member.mode & 0o111 else 0o644)
            seen.add(key)
    if seen != set(expected):
        raise ValueError('Package files missing from archive')
    info = plistlib.loads((destination / 'MCDMACX5Native.kext/Contents/Info.plist').read_bytes())
    if info.get('CFBundleVersion') != manifest.get('version') or info.get('CFBundleIdentifier') != 'org.mcdma.cx5.native':
        raise ValueError('Kernel extension identity mismatch')
    for key, name in [('kext_executable', 'MCDMACX5Native.kext/Contents/MacOS/MCDMACX5Native'),
                      ('kext_plist', 'MCDMACX5Native.kext/Contents/Info.plist'),
                      ('provider', 'libmcdma-rdmav34.so'), ('conf', 'mcdma.driver')]:
        if expected.get(name) != manifest.get('sha256', {}).get(key):
            raise ValueError('Required file identity mismatch')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive')
    parser.add_argument('destination')
    parser.add_argument('manifest')
    args = parser.parse_args()
    extract(args.archive, args.destination, json.loads(Path(args.manifest).read_text()))
