"""Bounded text-XML reader with original byte spans; never reserialize a tree."""
from dataclasses import dataclass
import re
import xml.parsers.expat
from .errors import MAX_BYTES, Unsupported
from .valuetree import Node, Scalar


@dataclass(frozen=True, slots=True)
class Document:
    root: Node
    encoding: str


def parse(data: bytes) -> Document:
    if len(data) > MAX_BYTES:
        raise Unsupported('XML exceeds 4 MiB limit')
    if data.startswith(b'\xff\xfe') or data.startswith(b'<\0'):
        encoding, width = 'utf-16-le', 2
    elif data.startswith(b'\xfe\xff') or data.startswith(b'\0<'):
        encoding, width = 'utf-16-be', 2
    else:
        encoding, width = 'utf-8', 1
    parser = xml.parsers.expat.ParserCreate()
    quotes = ('"'.encode(encoding), "'".encode(encoding))
    closing = '>'.encode(encoding)
    stack = []
    root = None
    budget = 20000

    def reject(*_):
        raise Unsupported('DTD and entity declarations are forbidden')

    def start(name, attrs):
        nonlocal budget
        budget -= 1 + len(attrs)
        if budget < 0 or len(stack) >= 64:
            raise Unsupported('XML resource limit')
        offset = parser.CurrentByteIndex
        # Lex only the parser-validated opening tag, keeping raw offsets/quotes.
        # Decode fixed-width units so byte indices remain exact for UTF-16/BOM.
        end = offset
        quote = None
        while end < len(data):
            unit = data[end:end + width]
            if quote:
                if unit == quote:
                    quote = None
            elif unit in quotes:
                quote = unit
            elif unit == closing:
                end += width
                break
            end += width
        else:
            raise Unsupported('Unterminated opening tag')
        tag = data[offset:end].decode(encoding, errors='strict')
        # Character offsets in UTF-8 need conversion; UTF-16 must account for
        # surrogate pairs too. Use encoded prefix lengths for either encoding.
        matches = list(re.finditer(r'''([^\s<>/=]+)\s*=\s*(["'])(.*?)\2''', tag, re.DOTALL))
        if len(matches) != len(attrs) or {m[1] for m in matches} != set(attrs):
            raise Unsupported('XML attribute spans are ambiguous')
        boundaries = sorted({p for m in matches for p in m.span(3)})
        byte_offsets = {}
        previous = 0
        byte_position = offset
        for position in boundaries:
            byte_position += len(tag[previous:position].encode(encoding))
            byte_offsets[position] = byte_position
            previous = position
        props = tuple(Scalar(m[1], 'string', attrs[m[1]],
                      byte_offsets[m.start(3)], byte_offsets[m.end(3)]) for m in matches)
        stack.append([name, props, [], []])

    def end(_name):
        nonlocal root
        name, props, children, parts = stack.pop()
        content = (Scalar(name, 'string', ''.join(p[0] for p in parts), parts[0][1], parts[-1][2])
                   if parts else None)
        node = Node(name, props, tuple(children), content)
        if stack:
            stack[-1][2].append(node)
        else:
            root = node

    def text(value):
        nonlocal budget
        if stack:
            budget -= 1
            if budget < 0:
                raise Unsupported('XML text event resource limit')
            start = parser.CurrentByteIndex
            stack[-1][3].append((value, start, start + len(value.encode(encoding))))

    def declaration(_version, declared, _standalone):
        if declared and declared.lower().replace('_', '-') not in ('utf-8', 'utf-16', 'utf-16le', 'utf-16be'):
            raise Unsupported('Only UTF-8 and UTF-16 XML encodings are supported')

    parser.StartElementHandler = start
    parser.EndElementHandler = end
    parser.CharacterDataHandler = text
    parser.StartDoctypeDeclHandler = reject
    parser.EntityDeclHandler = reject
    parser.ExternalEntityRefHandler = reject
    parser.XmlDeclHandler = declaration
    try:
        parser.Parse(data, True)
    except (xml.parsers.expat.ExpatError, UnicodeError) as exc:
        raise Unsupported('Malformed or unsupported text XML') from exc
    if root is None:
        raise Unsupported('Empty XML')
    return Document(root, encoding)
