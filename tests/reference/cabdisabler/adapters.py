"""Format registry and immutable scalar inspections. No plugin/version-specific mappings."""
from dataclasses import dataclass
from typing import Protocol
import xml.parsers.expat
from .errors import MAX_BYTES, Unsupported

@dataclass(frozen=True, slots=True)
class Inspection:
    adapter: str
    product: str
    version: str
    schema: str
    enabled: tuple[bool, ...]
    display_name: str | None
    cab_spans: tuple[tuple[int, int], ...]
    off_bytes: bytes | None = None
    on_bytes: bytes | None = None


class Adapter(Protocol):
    id: str

    def inspect(self, data: bytes) -> Inspection: ...
    def patch(self, data: bytes, enabled: bool) -> bytes: ...


def validate_inspection(info: Inspection, length: int, adapter_id: str) -> None:
    spans = sorted(info.cab_spans)
    if (info.adapter != adapter_id or not info.product or not info.enabled
            or any(type(value) is not bool for value in info.enabled)
            or len(spans) != len(info.enabled)):
        raise Unsupported('Inconsistent preset identity or whole-cab state')
    if (any(a < 0 or b <= a or b > length for a, b in spans)
            or any(b > c for (_, b), (c, _) in zip(spans, spans[1:]))):
        raise Unsupported('Invalid or overlapping cab scalar spans')


def looks_like_xml(data: bytes) -> bool:
    return (data.startswith((b'\xff\xfe', b'\xfe\xff', b'<\0', b'\0<'))
            or data.removeprefix(b'\xef\xbb\xbf').lstrip().startswith(b'<'))


@dataclass(frozen=True, slots=True)
class Registry:
    _adapters: tuple[Adapter, ...] = ()
    builtin: bool = False

    @property
    def adapters(self) -> tuple[Adapter, ...]:
        if self.builtin:
            from .formats import BUILTINS
            return BUILTINS
        return self._adapters

    def identify(self, data: bytes, product: str | None = None) -> tuple[Adapter, Inspection]:
        if len(data) > MAX_BYTES:
            raise Unsupported('Preset exceeds 4 MiB safety limit')
        if self.builtin:
            adapter = self.adapters[1 if looks_like_xml(data) else 0]
            info = adapter.inspect(data)
            validate_inspection(info, len(data), adapter.id)
            return adapter, info
        found = []
        for adapter in self.adapters:
            format_driven = getattr(adapter, 'format_driven', False)
            if not format_driven and product is not None and adapter.product != product:
                continue
            try:
                info = adapter.inspect(data)
            except Unsupported:
                continue
            validate_inspection(info, len(data), adapter.id)
            if not format_driven and (info.product != adapter.product or info.version != adapter.version):
                raise Unsupported('Adapter returned inconsistent identity')
            found.append((adapter, info))
        if len(found) != 1:
            raise Unsupported('Ambiguous adapters' if found else 'Unsupported preset format')
        return found[0]


PRODUCTION = Registry(builtin=True)


def representation(data: bytes) -> str:
    """Bounded classification, never a compatibility or encryption claim."""
    if len(data) > MAX_BYTES:
        return 'oversized'
    if not looks_like_xml(data):
        return 'binary / unknown (possibly encrypted or compressed)'
    parser = xml.parsers.expat.ParserCreate()
    count = depth = 0

    def reject(*_):
        raise Unsupported('DTD and entity declarations are forbidden')

    def start(*_):
        nonlocal count, depth
        count += 1
        depth += 1
        if count > 20000 or depth > 64:
            raise Unsupported('XML resource limit exceeded')

    def end(*_):
        nonlocal depth
        depth -= 1

    parser.StartElementHandler = start
    parser.EndElementHandler = end
    parser.StartDoctypeDeclHandler = reject
    parser.EntityDeclHandler = reject
    parser.ExternalEntityRefHandler = reject
    try:
        parser.Parse(data, True)
    except (xml.parsers.expat.ExpatError, Unsupported):
        return 'invalid / unsafe XML'
    return 'text XML (schema unverified)'
