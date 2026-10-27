"""Format-driven single-flag editing, independent of plugin names and versions."""
import re
import struct
from .adapters import Inspection
from .errors import Unsupported
from .valuetree import Node, Scalar, parse as parse_binary
from .xmltree import parse as parse_xml

WHOLE_CAB_KEYS = frozenset(('cabActive', 'cabSectionActive'))
# Legacy PARAM spelling, provisionally supported at the user's request.
# Match the exact parsed ID, never a suffix search or microphone-slot ID.
WHOLE_CAB_PARAM_IDS = WHOLE_CAB_KEYS | {'cabActiveID'}
GLOBAL_CONTAINERS = frozenset(('parameters', 'appModel', 'cabParameters'))


def _metadata(root: Node, names: tuple[str, ...]) -> str | None:
    # Metadata is descriptive, never an edit whitelist. Preserve unfamiliar
    # fields/types instead of rejecting a safe scalar because a version changed.
    for name in names:
        try:
            prop = root.property(name)
        except Unsupported:
            continue
        if prop.kind in ('string', 'int', 'int64', 'double'):
            return str(prop.value)
    return None


def _flag(root: Node) -> tuple[Scalar, str]:
    candidates = []
    stack = [(root, '', True)]
    while stack:
        node, parent_path, is_root = stack.pop()
        path = parent_path + '/' + node.name
        param_ids = set()
        for child in node.children:
            if child.name == 'PARAM':
                key = child.property('id')
                if key.kind != 'string' or not key.value or key.value in param_ids:
                    raise Unsupported('Duplicate or invalid PARAM identity')
                param_ids.add(key.value)
            stack.append((child, path, False))
        if node.name == 'PARAM':
            key = node.property('id')
            if key.value in WHOLE_CAB_PARAM_IDS:
                attributes = not node.children and {p.name for p in node.properties} == {'id', 'value'}
                elements = (not node.properties and len(node.children) == 2
                            and {c.name for c in node.children} == {'id', 'value'}
                            and all(not c.children and not c.properties for c in node.children))
                if not (attributes or elements) or node.text and node.text.value.strip():
                    raise Unsupported('Ambiguous whole-cab PARAM structure')
                candidates.append((node.property('value'), path + '[' + str(key.value) + ']'))
        else:
            for prop in node.properties:
                global_key = prop.name in WHOLE_CAB_KEYS and (is_root or node.name in GLOBAL_CONTAINERS)
                section_key = node.name == 'cabParameters' and prop.name == 'sectionActive'
                if global_key or section_key:
                    if node.text and node.text.value.strip():
                        raise Unsupported('Whole-cab property mixed with ambiguous text')
                    candidates.append((prop, path + '@' + prop.name))
    if len(candidates) != 1:
        raise Unsupported('Expected one unambiguous whole-cab flag; microphone/IR switches are excluded')
    return candidates[0]


def _string_states(text: str) -> tuple[bool, str, str]:
    for off, on in (('false', 'true'), ('False', 'True'), ('FALSE', 'TRUE')):
        if text in (off, on):
            return text == on, off, on
    if re.fullmatch(r'[01](?:\.0+)?', text):
        return text[0] == '1', '0' + text[1:], '1' + text[1:]
    raise Unsupported('Whole-cab flag must be a true/false or 0/1 scalar')


def _count(number: int) -> bytes:
    raw = number.to_bytes((number.bit_length() + 7) // 8, 'little')
    return bytes([len(raw)]) + raw


def _states(data: bytes, scalar: Scalar, encoding: str | None) -> tuple[bool, bytes, bytes]:
    raw = data[scalar.start:scalar.end]
    if scalar.kind == 'string':
        enabled, off, on = _string_states(str(scalar.value))
        if encoding:
            # Target entities are ambiguous lexical edits; unrelated entities are
            # parsed and preserved, but the flag itself must use literal values.
            if raw != str(scalar.value).encode(encoding):
                raise Unsupported('Whole-cab value uses entities or normalized whitespace')
            return enabled, off.encode(encoding), on.encode(encoding)
        def binary(text):
            payload = b'\x05' + text.encode('utf-8') + b'\0'
            return _count(len(payload)) + payload
        return enabled, binary(off), binary(on)
    if scalar.kind == 'bool':
        return bool(scalar.value), raw[:-1] + b'\x03', raw[:-1] + b'\x02'
    if scalar.kind in ('int', 'int64', 'double') and scalar.value in (0, 1):
        fmt = {'int': '<i', 'int64': '<q', 'double': '<d'}[scalar.kind]
        width = struct.calcsize(fmt)
        prefix = raw[:-width]
        return scalar.value == 1, prefix + struct.pack(fmt, 0), prefix + struct.pack(fmt, 1)
    raise Unsupported('Unsupported whole-cab value type or polarity')


class PresetFormat:
    format_driven = True
    version = 'structure detected from file; no plugin/version whitelist'
    evidence = 'Shared format rules; private legacy/modern samples and generated XML tests; plugin loads unverified'
    aliases = ()

    def __init__(self, text: bool):
        self.text = text
        self.id = 'text.xml' if text else 'binary.valuetree'
        self.product = 'Text XML presets' if text else 'Binary ValueTree presets'

    def inspect(self, data: bytes) -> Inspection:
        if self.text:
            document = parse_xml(data)
            root, encoding = document.root, document.encoding
        else:
            root, encoding = parse_binary(data), None
        scalar, locator = _flag(root)
        enabled, off, on = _states(data, scalar, encoding)
        return Inspection(self.id, root.name,
            _metadata(root, ('version', 'presetVersionProp')) or 'not recorded',
            locator + ':' + scalar.kind, (enabled,),
            _metadata(root, ('name', 'presetNameProp')), ((scalar.start, scalar.end),), off, on)

    def _patch_validated(self, data: bytes, enabled: bool, info: Inspection) -> bytes:
        if info.adapter != self.id or len(info.cab_spans) != 1:
            raise Unsupported('Wrong format inspection')
        if info.enabled == (enabled,):
            return data
        a, b = info.cab_spans[0]
        value = info.on_bytes if enabled else info.off_bytes
        if value is None:
            raise Unsupported('No validated flag encoding')
        return data[:a] + value + data[b:]

    def patch(self, data: bytes, enabled: bool) -> bytes:
        output = self._patch_validated(data, enabled, self.inspect(data))
        if self.inspect(output).enabled != (enabled,):
            raise Unsupported('Patched whole-cab flag failed validation')
        return output


BUILTINS = (PresetFormat(False), PresetFormat(True))
