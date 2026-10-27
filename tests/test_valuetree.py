"""Generated protocol examples, not uploaded presets or proprietary factory data."""
from pathlib import Path
import struct
import tempfile
import unittest
from cabdisabler.adapters import Registry, Unsupported
from cabdisabler.adapters import PRODUCTION
from cabdisabler.engine import Source, preview, apply
from cabdisabler.valuetree import parse, properties


def count(n):
    if n == 0:
        return b'\0'
    raw = n.to_bytes((n.bit_length() + 7) // 8, 'little')
    return bytes([len(raw)]) + raw


def value(item):
    if isinstance(item, str):
        raw = b'\x05' + item.encode('utf-8') + b'\0'
    elif isinstance(item, bool):
        raw = b'\x02' if item else b'\x03'
    elif isinstance(item, float):
        raw = b'\x04' + struct.pack('<d', item)
    elif isinstance(item, int):
        raw = b'\x01' + struct.pack('<i', item)
    elif isinstance(item, bytes):
        raw = b'\x08' + item
    else:
        raise TypeError(item)
    return count(len(raw)) + raw


def node(name, props=(), children=()):
    return (name.encode() + b'\0' + count(len(props)) +
            b''.join(key.encode() + b'\0' + value(v) for key, v in props) +
            count(len(children)) + b''.join(children))


def nolly(enabled=True, version='1.0.2', duplicate=False):
    cab = node('cabParameters', [('sectionActive', 'true' if enabled else 'false'),
                                 ('leftMicActive', 'false'), ('irPath', '/private/IR.wav')])
    params = node('parameters', [], [node('subModels', [], [cab] * (2 if duplicate else 1))])
    app = node('appModel', [], [node('subModels', [], [params])])
    return node('nolly-x', [('version', version), ('name', '音'), ('isFavorite', False)], [app])


def legacy(enabled=True, flags=('cabActive',), version='0.9.6'):
    children = [node('PARAM', [('id', flag), ('value', float(enabled))]) for flag in flags]
    children += [node('PARAM', [('id', 'cab1Active'), ('value', 0.0)]),
                 node('PARAM', [('id', 'driveGain'), ('value', 0.375)])]
    return node('neural_dsp_test_legacy', [('presetVersionProp', version), ('presetNameProp', 'Legacy 音'),
                ('presetPathProp', '/unchanged/Tone.xml')], children)


class ValueTreeTests(unittest.TestCase):
    def test_typed_values_and_utf8(self):
        data = node('example', [('name', '日本語'), ('bool', True), ('double', 0.25),
                               ('int', -12), ('bytes', b'\0\xff')])
        root = parse(data)
        self.assertEqual(root.property('name').value, '日本語')
        self.assertEqual(root.property('bool').kind, 'bool')
        self.assertEqual(root.property('double').value, 0.25)
        self.assertEqual(root.property('bytes').value, b'\0\xff')

    def test_truncation_at_every_offset(self):
        data = nolly()
        for i in range(len(data)):
            with self.subTest(offset=i), self.assertRaises(Unsupported):
                parse(data[:i])

    def test_duplicates_and_trailing_bytes(self):
        for data in (node('x', [('key', 1), ('key', 2)]), nolly() + b'junk'):
            with self.assertRaises(Unsupported):
                parse(data)
        with self.assertRaises(Unsupported):
            properties(parse(nolly(duplicate=True)))

    def test_invalid_lengths_and_types(self):
        cases = [b'root\0\x81\x01', b'root\0\x05', b'root\0\x01\xff',
                 b'root\0\x01\x01key\0\x01\x02\xff\0',
                 b'root\0\x01\x01key\0\x01\x02\x02\0\0']
        for data in cases:
            with self.subTest(data=data), self.assertRaises(Unsupported):
                parse(data)

    def test_array_int64_void_and_undefined(self):
        array = b'\x07' + count(2) + value('音') + value(False)
        encoded = count(len(array)) + array
        raw = (b'root\0' + count(4) + b'items\0' + encoded +
               b'large\0' + b'\x01\x09\x06' + struct.pack('<q', -9223372036854775808) +
               b'empty\0\0' + b'undefined\0\x01\x01\x09' + b'\0')
        tree = parse(raw)
        self.assertEqual([v.value for v in tree.property('items').value], ['音', False])
        self.assertEqual(tree.property('large').value, -9223372036854775808)
        self.assertEqual(tree.property('empty').kind, 'void')
        self.assertEqual(tree.property('undefined').kind, 'undefined')

    def test_resource_limits(self):
        data = node('end')
        for _ in range(66):
            data = node('nested', [], [data])
        for bad in (data, b'x' * (4*1024*1024+1), b'x'*65537+b'\0\0\0'):
            with self.assertRaises(Unsupported):
                parse(bad)

    def test_nolly_only_one_variable_length_scalar(self):
        before = nolly()
        # Generated minimal schema is accepted only by this test-local instance.
        adapter = PRODUCTION.adapters[0]
        info = adapter.inspect(before)
        after = adapter.patch(before, False)
        check = adapter.inspect(after)
        a,b = info.cab_spans[0]; c,d = check.cab_spans[0]
        self.assertEqual(before[:a], after[:c])
        self.assertEqual(before[b:], after[d:])
        self.assertEqual(len(after), len(before)+1)
        self.assertEqual(adapter.patch(after, True), before)
        self.assertEqual(adapter.patch(after, False), after)
        old, new = properties(parse(before)), properties(parse(after))
        changed = [k for k in old if old[k].value != new[k].value]
        self.assertEqual(len(changed), 1)
        self.assertTrue(changed[0].endswith('cabParameters@sectionActive'))

    def test_new_products_versions_and_additional_settings_need_no_mapping(self):
        data = nolly(version='123.456.7').replace(b'nolly-x', b'unseen-archetype-x')
        adapter, info = PRODUCTION.identify(data, 'Previously untested product')
        self.assertEqual(info.product, 'unseen-archetype-x')
        self.assertEqual(info.version, '123.456.7')
        self.assertEqual(adapter.patch(adapter.patch(data, False), True), data)

    def test_engine_accepts_only_flag_length_change(self):
        before = nolly()
        adapter = PRODUCTION.adapters[0]
        with tempfile.TemporaryDirectory() as d:
            p = Path(d).resolve() / 'Tone.xml'
            p.write_bytes(before)
            registry = Registry((adapter,))
            def plan(op):
                return preview((Source(p, 'Archetype Nolly X', p.parent),), op, registry=registry)
            self.assertEqual(apply(plan('disable'))[0].status, 'changed')
            self.assertEqual(apply(plan('enable'))[0].status, 'changed')
            self.assertEqual(p.read_bytes(), before)

    def test_engine_rejects_tone_change_with_valid_new_length(self):
        data = nolly()
        class Bad:
            def __init__(self, base): self.base = base
            def __getattr__(self, name): return getattr(self.base, name)
            def patch(self, data, enabled):
                return self.base.patch(data, enabled).replace(b'/private/IR.wav', b'/private/XX.wav')
            def _patch_validated(self, data, enabled, info):
                return self.patch(data, enabled)
        base = PRODUCTION.adapters[0]
        with tempfile.TemporaryDirectory() as d:
            p = Path(d).resolve() / 'Tone.xml'; p.write_bytes(data)
            plan = preview((Source(p, 'Archetype Nolly X', p.parent),), 'disable', registry=Registry((Bad(base),)))
            self.assertEqual(plan.items[0].status, 'skipped')
            self.assertIn('outside validated cab spans', plan.items[0].reason)
            self.assertEqual(p.read_bytes(), data)

    def test_shared_non_x_layout_and_exact_roundtrip(self):
        adapter = PRODUCTION.adapters[0]
        for flag in ('cabActive', 'cabSectionActive'):
            before = legacy(flags=(flag,))
            info = adapter.inspect(before)
            after = adapter.patch(before, False)
            a,b = info.cab_spans[0]
            self.assertEqual(before[:a], after[:a])
            self.assertEqual(before[b:], after[b:])
            self.assertEqual(adapter.patch(after, True), before)
            self.assertEqual(adapter.patch(after, False), after)
            old,new = properties(parse(before)),properties(parse(after))
            changed = [k for k in old if old[k].value != new[k].value]
            self.assertEqual(len(changed), 1)
            self.assertIn('PARAM[' + flag + ']@value', changed[0])

    def test_shared_layout_rejects_mic_only_duplicate_and_ambiguous(self):
        for flags in ((), ('cabActive','cabActive'), ('cabActive','cabSectionActive')):
            with self.assertRaises(Unsupported):
                PRODUCTION.adapters[0].inspect(legacy(flags=flags))

    def test_shared_layout_rejects_wrong_value_and_structure(self):
        for data in (legacy().replace(struct.pack('<d', 1.0),struct.pack('<d', 0.5),1),
                     node('unrelated', [('presetVersionProp', '1.0.0')])):
            with self.assertRaises(Unsupported):
                PRODUCTION.adapters[0].inspect(data)

    def test_all_archetype_labels_use_the_same_format_detection(self):
        for product in ('Archetype Original', 'Archetype Original X', 'Brand new Archetype'):
            for data in (legacy(), nolly()):
                adapter, info = PRODUCTION.identify(data, product)
                self.assertEqual(adapter.id, 'binary.valuetree')
                self.assertEqual(info.enabled, (True,))

    def test_ambiguous_modern_sections_rejected(self):
        with self.assertRaises(Unsupported):
            PRODUCTION.identify(nolly(duplicate=True))
