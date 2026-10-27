"""Verify ad hoc CodeDirectory page/resource hashes on Linux, without CMS identity claims."""
import hashlib
import os
from pathlib import Path
import plistlib
import struct

from macholib.MachO import MachO


def verify_binary(path, external=None):
    content = Path(path).read_bytes()
    directories = []
    for header in MachO(str(path)).headers:
        external_slots = dict(external or {})
        for _, _, sections in header.commands:
            if isinstance(sections, list):
                for section in sections:
                    if section.sectname.rstrip(b'\0') == b'__info_plist':
                        start = header.offset + section.offset
                        external_slots.setdefault(1, content[start:start + section.size])
        signatures = [command for lc, command, _ in header.commands if lc.cmd == 29]
        if len(signatures) != 1:
            raise ValueError(f'Expected one signature per architecture: {path}')
        command = signatures[0]
        start = header.offset + command.dataoff
        signature = content[start:start + command.datasize]
        magic, length, count = struct.unpack_from('>III', signature)
        if magic != 0xfade0cc0 or length > len(signature):
            raise ValueError('Invalid signature superblob')
        blobs = {}
        for index in range(count):
            slot, offset = struct.unpack_from('>II', signature, 12 + index * 8)
            _, size = struct.unpack_from('>II', signature, offset)
            if offset + size > length:
                raise ValueError('Signature blob exceeds superblob')
            blobs[slot] = signature[offset:offset + size]
        if 0 not in blobs:
            raise ValueError('Missing primary CodeDirectory')
        for slot, data in blobs.items():
            if slot != 0 and not 0x1000 <= slot <= 0x1005:
                continue
            fields = struct.unpack_from('>9I4B', data)
            magic, size, version, flags, hashes, _, special, pages, limit, hash_size, kind, _, page_bits = fields
            if magic != 0xfade0c02 or size != len(data) or not flags & 2:
                raise ValueError('Expected an ad hoc CodeDirectory')
            if version >= 0x20100 and struct.unpack_from('>I', data, 44)[0]:
                raise ValueError('Scattered signatures are not supported by this gate')
            algorithm = {1: 'sha1', 2: 'sha256', 3: 'sha256', 4: 'sha384'}.get(kind)
            if not algorithm or page_bits > 20 or not page_bits:
                raise ValueError('Unsupported code-signature digest/page size')
            if hashes < special * hash_size or hashes + pages * hash_size > len(data):
                raise ValueError('Invalid signature hash bounds')
            page_size = 1 << page_bits
            if limit != command.dataoff or pages != (limit + page_size - 1) // page_size:
                raise ValueError('Signature does not cover the whole code region')
            def digest(raw):
                return hashlib.new(algorithm, raw).digest()[:hash_size]
            for index in range(pages):
                block = content[header.offset + index * page_size:header.offset + min((index + 1) * page_size, limit)]
                if digest(block) != data[hashes + index * hash_size:hashes + (index + 1) * hash_size]:
                    raise ValueError(f'Code page hash mismatch: {path}, page {index}')
            for index in range(1, special + 1):
                expected = data[hashes - index * hash_size:hashes - (index - 1) * hash_size]
                if not any(expected):
                    continue
                raw = external_slots.get(index, blobs.get(index))
                if raw is None or digest(raw) != expected:
                    raise ValueError(f'Special slot {index} hash mismatch: {path}')
            directories.append(hashlib.new(algorithm, data).digest()[:20])
    return directories


def verify_app(app, paths):
    app = Path(app)
    root = app / 'Contents'
    info = root / 'Info.plist'
    resources = root / '_CodeSignature/CodeResources'
    executable = root / 'MacOS' / plistlib.loads(info.read_bytes())['CFBundleExecutable']
    external_by_executable = {executable: {1: info.read_bytes(), 3: resources.read_bytes()}}
    sealed = [(root, plistlib.loads(resources.read_bytes()))]
    hashes = {path: verify_binary(path, external_by_executable.get(path)) for path in paths}
    for root, manifest in sealed:
        for relative, entry in manifest['files2'].items():
            path = root / relative
            if not path.resolve().is_relative_to(app.resolve()):
                raise ValueError('Sealed resource escapes bundle')
            if 'hash2' in entry and hashlib.sha256(path.read_bytes()).digest() != entry['hash2']:
                raise ValueError(f'Resource hash mismatch: {path}')
            if 'symlink' in entry and (not path.is_symlink() or os.readlink(path) != entry['symlink']):
                raise ValueError(f'Resource symlink mismatch: {path}')
            if 'cdhash' in entry:
                if not path.is_file():
                    raise ValueError(f'Unexpected nested code bundle in native app: {path}')
                executable = path
                if entry['cdhash'] not in hashes.get(executable, []):
                    raise ValueError(f'Nested code hash mismatch: {path}')
    return len(hashes)
