"""Strict, bounded JUCE ValueTree inspection. Parsing never establishes edit support.

Original implementation of the documented stream layout, not a permissive key scan.
No decryption, decompression, format guessing, or whole-tree serialization.
"""
from dataclasses import dataclass
import math
import re
import struct
from .errors import MAX_BYTES, Unsupported

_CONTROL = re.compile(rb'[\x00-\x1f]')
_NUMERIC = {1: (4, '<i', 'int'), 4: (8, '<d', 'double'), 6: (8, '<q', 'int64')}


@dataclass(frozen=True, slots=True)
class Scalar:
    name: str
    kind: str
    value: object
    start: int  # Entire encoded var, including compressed length and type marker
    end: int


@dataclass(frozen=True, slots=True)
class Node:
    name: str
    properties: tuple[Scalar, ...]
    children: tuple['Node', ...]
    text: Scalar | None = None

    def property(self, name: str) -> Scalar:
        found = [p for p in self.properties if p.name == name]
        found.extend(c.text for c in self.children
                     if c.name == name and c.text is not None and not c.children and not c.properties)
        if len(found) != 1:
            raise Unsupported(f'Expected exactly one property {name!r}')
        return found[0]



class Reader:
    def __init__(self, data: bytes):
        if len(data) > MAX_BYTES:
            raise Unsupported('ValueTree exceeds 4 MiB limit')
        self.data = data
        self.size = len(data)
        self.offset = 0
        self.budget = 20000

    def take(self, count: int, limit: int | None = None) -> bytes:
        end = self.offset + count
        if count < 0 or end > self.size or limit is not None and end > limit:
            raise Unsupported('Truncated ValueTree')
        result = self.data[self.offset:end]
        self.offset = end
        return result

    def count(self, limit: int | None = None) -> int:
        if self.offset >= self.size or limit is not None and self.offset >= limit:
            raise Unsupported('Truncated compressed length')
        size = self.data[self.offset]
        self.offset += 1
        if size & 0x80 or size > 4:
            raise Unsupported('Negative or invalid compressed length')
        end = self.offset + size
        if end > self.size or limit is not None and end > limit:
            raise Unsupported('Truncated compressed length')
        if size == 0:
            return 0
        value = self.data[self.offset] if size == 1 else int.from_bytes(self.data[self.offset:end], 'little')
        self.offset = end
        if value > MAX_BYTES:
            raise Unsupported('ValueTree length exceeds limit')
        return value

    def text(self) -> str:
        end = self.data.find(b'\0', self.offset, self.offset + 65537)
        if end < 0:
            raise Unsupported('Missing terminator or oversized ValueTree identifier')
        raw = self.data[self.offset:end]
        self.offset = end + 1
        try:
            text = raw.decode('utf-8', errors='strict')
        except UnicodeDecodeError as exc:
            raise Unsupported('Invalid UTF-8 identifier') from exc
        if not text or _CONTROL.search(raw):
            raise Unsupported('Empty or invalid ValueTree identifier')
        return text

    def scalar(self, name: str, depth: int, limit: int | None = None) -> Scalar:
        self.budget -= 1
        if self.budget < 0 or depth > 64:
            raise Unsupported('ValueTree resource limit')
        start = self.offset
        size = self.count(limit)
        end = self.offset + size
        if end > self.size or limit is not None and end > limit:
            raise Unsupported('Truncated scalar')
        if size == 0:
            return Scalar(name, 'void', None, start, self.offset)
        marker = self.data[self.offset]
        self.offset += 1
        payload = size - 1
        if marker in _NUMERIC:
            expected, fmt, kind = _NUMERIC[marker]
            if payload != expected:
                raise Unsupported('Invalid numeric scalar length')
            value = struct.unpack_from(fmt, self.data, self.offset)[0]
            self.offset = end
            if isinstance(value, float) and not math.isfinite(value):
                raise Unsupported('Non-finite scalar')
        elif marker in (2, 3, 9):
            if payload != 0:
                raise Unsupported('Invalid boolean/undefined scalar length')
            kind, value = ('undefined', None) if marker == 9 else ('bool', marker == 2)
        elif marker == 5:
            raw = self.take(payload, end)
            if not raw.endswith(b'\0') or b'\0' in raw[:-1]:
                raise Unsupported('Invalid string terminator')
            try:
                value = raw[:-1].decode('utf-8', errors='strict')
            except UnicodeDecodeError as exc:
                raise Unsupported('Invalid UTF-8 scalar') from exc
            kind = 'string'
        elif marker == 8:
            value, kind = self.take(payload, end), 'binary'
        elif marker == 7:
            count = self.count(end)
            if count > self.budget:
                raise Unsupported('Array resource limit')
            value = tuple(self.scalar(str(i), depth + 1, end) for i in range(count))
            kind = 'array'
        else:
            raise Unsupported(f'Unknown scalar type {marker}')
        if self.offset != end:
            raise Unsupported('Scalar length mismatch')
        return Scalar(name, kind, value, start, end)

    def node(self, depth: int = 0) -> Node:
        self.budget -= 1
        if self.budget < 0 or depth > 64:
            raise Unsupported('ValueTree resource limit')
        name = self.text()
        count = self.count()
        if count > self.budget:
            raise Unsupported('Property resource limit')
        props = []
        names = set()
        for _ in range(count):
            key = self.text()
            if key in names:
                raise Unsupported('Duplicate property')
            names.add(key)
            props.append(self.scalar(key, depth))
        count = self.count()
        if count > self.budget:
            raise Unsupported('Child resource limit')
        children = tuple(self.node(depth + 1) for _ in range(count))
        return Node(name, tuple(props), children)


def parse(data: bytes) -> Node:
    reader = Reader(data)
    result = reader.node()
    if reader.offset != len(data):
        raise Unsupported('Trailing bytes after ValueTree; no schema established')
    return result


def properties(node: Node, prefix: str = '') -> dict[str, Scalar]:
    """Stable semantic paths for comparison; distinguish repeated legacy PARAM nodes."""
    path = prefix + '/' + node.name
    if node.name == 'PARAM':
        identifier = node.property('id')
        if identifier.kind != 'string':
            raise Unsupported('Invalid PARAM identity')
        path += '[' + str(identifier.value) + ']'
    result = {path + '@' + prop.name: prop for prop in node.properties}
    for child in node.children:
        incoming = properties(child, path)
        if result.keys() & incoming.keys():
            raise Unsupported('Ambiguous repeated semantic path')
        result.update(incoming)
    return result
