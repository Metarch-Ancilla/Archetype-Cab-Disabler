"""Losslessly recompress ICO PNG payloads without removing icon sizes.

Optional asset tools: Pillow and zopfli, never shipped or run by app builders.
"""
import argparse
from pathlib import Path
import struct
from icon_compression import compress_png


def optimize(data):
    reserved, kind, count = struct.unpack_from('<HHH', data)
    if reserved or kind != 1 or not count or 6 + 16 * count > len(data):
        raise ValueError('Invalid ICO header')
    records = [];payloads = [];cache = {};offset = 6 + 16 * count
    for index in range(count):
        entry = data[6 + index * 16:22 + index * 16]
        size, start = struct.unpack_from('<II', entry, 8)
        if start < 6 + 16 * count or not size or start + size > len(data):
            raise ValueError('Invalid ICO image span')
        payload = compress_png(data[start:start + size], cache)
        records.append(entry[:8] + struct.pack('<II', len(payload), offset))
        payloads.append(payload);offset += len(payload)
    return data[:6] + b''.join(records + payloads)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('icon', type=Path);args = parser.parse_args()
    before = args.icon.read_bytes();after = optimize(before);args.icon.write_bytes(after)
    print(f'{args.icon}: {len(before)} -> {len(after)} bytes; all sizes and RGBA pixels preserved')
