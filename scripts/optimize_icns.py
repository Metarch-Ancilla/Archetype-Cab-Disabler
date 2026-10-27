"""Losslessly recompress ICNS PNG payloads, retaining every representation/pixel.

Optional asset-maintenance tools: Pillow and zopfli (not app/build dependencies).
"""
import argparse
from pathlib import Path
import struct
from icon_compression import compress_png


def optimize(data):
    if data[:4] != b'icns' or struct.unpack('>I', data[4:8])[0] != len(data):
        raise ValueError('Invalid ICNS header')
    chunks = [];offset = 8;cache = {}
    while offset < len(data):
        kind, size = struct.unpack_from('>4sI', data, offset)
        if size < 8 or offset + size > len(data):
            raise ValueError('Invalid ICNS chunk')
        payload = data[offset + 8:offset + size];offset += size
        payload = compress_png(payload, cache)
        chunks.append((kind, payload))
    # The table of contents stores chunk sizes and must reflect recompression.
    records = b''.join(kind + struct.pack('>I', len(payload) + 8)
                       for kind, payload in chunks if kind != b'TOC ')
    body = b''.join(kind + struct.pack('>I', len(records if kind == b'TOC ' else payload) + 8) +
                    (records if kind == b'TOC ' else payload) for kind, payload in chunks)
    return b'icns' + struct.pack('>I', len(body) + 8) + body


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('icon', type=Path);args = parser.parse_args()
    before = args.icon.read_bytes();after = optimize(before)
    args.icon.write_bytes(after)
    print(f'{args.icon}: {len(before)} -> {len(after)} bytes; all representations and RGBA pixels preserved')
