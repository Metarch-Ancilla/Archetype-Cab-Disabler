"""Invented formats for exercising safety mechanisms, NEVER Neural DSP adapters."""
import re
import xml.etree.ElementTree as ET
from cabdisabler.adapters import Inspection, Unsupported, representation


class SyntheticXML:
    id = 'test.xml'
    product = 'TEST ONLY'
    version = '1'
    evidence = 'Synthetic fixture, not a real plugin schema'

    def inspect(self, data):
        if representation(data) != 'text XML (schema unverified)':
            raise Unsupported('Not safe XML')
        root = ET.fromstring(data)
        if root.tag != 'test-preset' or root.attrib != {'product': self.product, 'version': '1'}:
            raise Unsupported('Unknown synthetic schema')
        names = root.findall('name')
        cabs = root.findall('cab')
        if len(names) != 1 or len(cabs) != 2 or len(root.findall('.//cab')) != 2:
            raise Unsupported('Ambiguous name/cab nodes')
        spans = tuple((m.start(1), m.end(1)) for m in re.finditer(rb'<cab>([01])</cab>', data))
        if len(spans) != 2 or any(c.text not in ('0', '1') for c in cabs):
            raise Unsupported('Invalid cab encoding')
        return Inspection(self.id, self.product, self.version, 'synthetic.xml.v1',
                          tuple(c.text == '1' for c in cabs), names[0].text, spans)

    def patch(self, data, enabled):
        info = self.inspect(data)
        out = bytearray(data)
        for a, b in info.cab_spans:
            out[a:b] = b'1' if enabled else b'0'
        return bytes(out)



class SyntheticBinary:
    id = 'test.binary'
    product = 'TEST BINARY'
    version = '1'
    evidence = 'Invented binary test format; no Neural DSP compatibility'

    def inspect(self, data):
        if len(data) != 20 or data[:8] != b'CABTEST0' or any(x not in (0, 1) for x in data[8:12]):
            raise Unsupported('Invalid synthetic binary')
        return Inspection(self.id, self.product, self.version, 'synthetic.binary.v1',
                          tuple(bool(x) for x in data[8:10]), None, ((8, 9), (9, 10)))

    def patch(self, data, enabled):
        self.inspect(data)
        return data[:8] + bytes([int(enabled)] * 2) + data[10:]


def xml(name='Tone', state=b'1', second=b'1'):
    return (b'\xef\xbb\xbf<?xml version="1.0" encoding="UTF-8"?>\r\n'
            b'<test-preset product="TEST ONLY" version="1"><name>' + name.encode() +
            b'</name><cab>' + state + b'</cab><mic>0</mic><cab>' + second +
            b'</cab><ir path="custom.wav"/><gain>0.21</gain></test-preset>\r\n')
