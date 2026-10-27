from dataclasses import replace
from pathlib import Path
import os
import tempfile
import threading
import unittest
from unittest.mock import patch
from cabdisabler.adapters import Registry, Unsupported, representation
from cabdisabler.engine import Source, preview, apply, read_preset
from tests.fixtures import SyntheticXML, SyntheticBinary, xml


class EngineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.library = self.root / 'library'
        self.library.mkdir()
        self.source = self.library / 'Tone.xml'
        self.source.write_bytes(xml())
        self.registry = Registry((SyntheticXML(), SyntheticBinary()))

    def plan(self, operation='disable', files=None):
        return preview(tuple(Source(p, 'TEST ONLY', self.library) for p in files or [self.source]),
                       operation, self.root / 'user', self.registry)

    def test_unknown_input_never_writes(self):
        p = preview((Source(self.source, 'Archetype Petrucci', self.library),), 'disable')
        self.assertEqual(p.items[0].status, 'skipped')
        self.assertEqual(apply(p)[0].status, 'skipped')
        self.assertEqual(self.source.read_bytes(), xml())

    def test_preview_is_immutable_and_no_writes(self):
        p = self.plan()
        self.assertEqual(self.source.read_bytes(), xml())
        with self.assertRaises(AttributeError):
            p.operation = 'enable'
        self.assertEqual(p.items[0].before, (True, True))
        self.assertEqual(p.items[0].after, (False, False))

    def test_disable_enable_exact_roundtrip(self):
        original = self.source.read_bytes()
        p = self.plan()
        self.assertEqual(apply(p)[0].status, 'changed')
        changed = self.source.read_bytes()
        info = self.registry.adapters[0].inspect(original)
        allowed = {a for a, b in info.cab_spans}
        self.assertTrue(all(a == b or i in allowed for i, (a, b) in enumerate(zip(original, changed))))
        self.assertIn(b'<mic>0</mic>', changed)
        self.assertEqual(apply(self.plan('enable'))[0].status, 'changed')
        self.assertEqual(self.source.read_bytes(), original)

    def test_disable_idempotent(self):
        apply(self.plan())
        self.assertEqual(apply(self.plan())[0].status, 'already-correct')

    def test_enable_is_stateless(self):
        self.source.write_bytes(xml(state=b'0', second=b'0'))
        self.assertEqual(apply(self.plan('enable'))[0].status, 'changed')
        self.assertEqual(self.source.read_bytes(), xml())
        self.assertEqual(apply(self.plan('enable'))[0].status, 'already-correct')

    def test_binary_two_cabs_mic_preservation(self):
        original = b'CABTEST0' + b'\x01\x01\x00\x01' + b'IRPATH!!'
        self.source.write_bytes(original)
        def run(op):
            return preview((Source(self.source, 'TEST BINARY', self.library),), op, registry=self.registry)
        p = run('disable')
        self.assertEqual(apply(p)[0].status, 'changed')
        self.assertEqual(self.source.read_bytes()[10:], original[10:])
        apply(run('enable'))
        self.assertEqual(self.source.read_bytes(), original)

    def test_structurally_valid_binary_can_be_copied(self):
        self.source.write_bytes(b'CABTEST0' + b'\x01\x01\x00\x01' + b'IRPATH!!')
        p = preview((Source(self.source, 'TEST BINARY', self.library),), 'copy', self.root/'user', self.registry)
        self.assertEqual(p.items[0].status, 'ready')
        self.assertEqual(apply(p)[0].status, 'changed')
        self.assertEqual(self.source.read_bytes(), b'CABTEST0' + b'\x01\x01\x00\x01' + b'IRPATH!!')

    def test_copy_unicode_nested_suffix_source_immutable(self):
        nested = self.library / 'Factory' / '日本語'
        nested.mkdir(parents=True)
        src = nested / '音.xml'
        original = xml('音')
        src.write_bytes(original)
        p = self.plan('copy', [src])
        self.assertEqual(apply(p)[0].status, 'changed')
        target = self.root / 'user' / 'TEST ONLY' / 'Factory' / '日本語' / '音.xml'
        self.assertEqual(target, p.items[0].destination)
        self.assertIn('<name>音</name>'.encode(), target.read_bytes())
        self.assertNotIn('音 CD'.encode(), target.read_bytes())
        self.assertEqual(src.read_bytes(), original)
        self.assertEqual(apply(self.plan('copy', [src]))[0].status, 'already-correct')

    def test_existing_cd_name_is_preserved(self):
        src = self.library / 'Tone CD.xml'
        src.write_bytes(xml('Tone CD', b'0', b'0'))
        p = self.plan('copy', [src])
        self.assertEqual(p.items[0].destination.name, src.name)

    def test_copy_enable_preserves_original_and_never_clobbers(self):
        apply(self.plan('disable'))
        original = self.source.read_bytes()
        plan = self.plan('copy-enable')
        target = plan.items[0].destination
        self.assertNotEqual(target, self.source)
        self.assertEqual(apply(plan)[0].status, 'changed')
        self.assertEqual(self.source.read_bytes(), original)
        self.assertEqual(target.read_bytes(), xml())
        self.assertEqual(self.plan('copy-enable').items[0].status, 'already-correct')
        target.unlink()
        plan = self.plan('copy-enable')
        target.write_bytes(b'new preset at destination')
        self.assertEqual(apply(plan)[0].status, 'failed')
        self.assertEqual(target.read_bytes(), b'new preset at destination')
        self.assertEqual(self.source.read_bytes(), original)

    def test_enable_copy_retains_suffix(self):
        p = self.plan('copy')
        apply(p)
        target = p.items[0].destination
        apply(self.plan('enable', [target]))
        self.assertIn(b'<name>Tone</name>', target.read_bytes())
        self.assertEqual(SyntheticXML().inspect(target.read_bytes()).enabled, (True, True))

    def test_different_copy_collision(self):
        p = self.plan('copy')
        apply(p)
        target = p.items[0].destination
        target.write_bytes(b'unrelated')
        self.assertEqual(self.plan('copy').items[0].status, 'skipped')
        self.assertEqual(target.read_bytes(), b'unrelated')

    def test_destination_race_does_not_overwrite(self):
        p = self.plan('copy')
        target = p.items[0].destination
        target.parent.mkdir(parents=True)
        target.write_bytes(b'new')
        self.assertEqual(apply(p)[0].status, 'failed')
        self.assertEqual(target.read_bytes(), b'new')

    def test_atomic_copy_race_does_not_overwrite(self):
        p = self.plan('copy')
        real_link = os.link
        def racing_link(src, dst):
            Path(dst).write_bytes(b'concurrent')
            real_link(src, dst)
        with patch('cabdisabler.engine.os.link', side_effect=racing_link):
            self.assertEqual(apply(p)[0].status, 'failed')
        self.assertEqual(p.items[0].destination.read_bytes(), b'concurrent')
        self.assertFalse(list(self.root.rglob('.cab-*')))

    def test_source_changed_after_preview(self):
        p = self.plan()
        self.source.write_bytes(xml('New'))
        self.assertEqual(apply(p)[0].status, 'failed')
        self.assertEqual(self.source.read_bytes(), xml('New'))

    def test_same_bytes_replaced_since_preview(self):
        p = self.plan()
        replacement = self.library / 'replacement'
        replacement.write_bytes(xml())
        os.replace(replacement, self.source)
        self.assertEqual(apply(p)[0].status, 'failed')
        self.assertEqual(self.source.read_bytes(), xml())

    def test_change_during_write(self):
        p = self.plan()
        def change(_):
            self.source.write_bytes(xml('Concurrent'))
        with patch('cabdisabler.engine.os.fsync', side_effect=change):
            self.assertEqual(apply(p)[0].status, 'failed')
        self.assertEqual(self.source.read_bytes(), xml('Concurrent'))
        self.assertFalse(list(self.root.rglob('.cab-*')))

    def test_cancel_before_first_file(self):
        event = threading.Event()
        event.set()
        self.assertEqual(apply(self.plan(), event)[0].status, 'cancelled')
        self.assertEqual(self.source.read_bytes(), xml())

    def test_cancel_between_files_partial_results(self):
        other = self.library / 'Other.xml'
        other.write_bytes(xml('Other'))
        event = threading.Event()
        result = apply(self.plan(files=[self.source, other]), event, lambda _: event.set())
        self.assertEqual([r.status for r in result], ['changed', 'cancelled'])
        self.assertEqual(other.read_bytes(), xml('Other'))

    def test_direct_write_failure_leaves_source_untouched(self):
        with patch('cabdisabler.engine.write_original', side_effect=PermissionError('denied')):
            self.assertEqual(apply(self.plan())[0].status, 'failed')
        self.assertEqual(self.source.read_bytes(), xml())
        self.assertFalse(list(self.root.rglob('.cab-*')))

    def test_temp_creation_failure(self):
        with patch('cabdisabler.engine.tempfile.mkstemp', side_effect=PermissionError('denied')):
            self.assertEqual(apply(self.plan('copy'))[0].status, 'failed')
        self.assertEqual(self.source.read_bytes(), xml())

    @unittest.skipIf(os.name == 'nt', 'FIFO is POSIX only')
    def test_fifo_rejected_without_blocking(self):
        fifo = self.library / 'pipe.xml'
        os.mkfifo(fifo)
        with self.assertRaises(Unsupported):
            read_preset(fifo)

    def test_plan_memory_budget(self):
        with patch('cabdisabler.engine.MAX_PLAN_BYTES', 1):
            self.assertEqual(self.plan().items[0].status, 'skipped')
        self.assertEqual(self.source.read_bytes(), xml())

    def test_preview_permission_failure(self):
        with patch('cabdisabler.engine.os.access', return_value=False):
            self.assertEqual(self.plan().items[0].status, 'failed')

    def test_inplace_preserves_mode(self):
        self.source.chmod(0o640)
        before = self.source.stat()
        apply(self.plan())
        after = self.source.stat()
        self.assertEqual((after.st_dev, after.st_ino, after.st_uid, after.st_gid),
                         (before.st_dev, before.st_ino, before.st_uid, before.st_gid))
        if os.name != 'nt':
            self.assertEqual(self.source.stat().st_mode & 0o777, 0o640)

    def test_direct_edit_needs_only_file_write_access(self):
        with patch('cabdisabler.engine.os.access', side_effect=lambda path, mode: path == self.source), \
             patch('cabdisabler.engine.tempfile.mkstemp', side_effect=AssertionError('No temporary file for Direct Edit')):
            plan = self.plan()
            self.assertEqual(plan.items[0].status, 'ready')
            self.assertEqual(apply(plan)[0].status, 'changed')
        self.assertEqual(SyntheticXML().inspect(self.source.read_bytes()).enabled, (False, False))

    def test_hardlink_deduplicated(self):
        alias = self.library / 'Alias.xml'
        os.link(self.source, alias)
        self.assertEqual(len(self.plan(files=[self.source, alias]).items), 1)

    @unittest.skipIf(os.name == 'nt', 'Symlink permissions vary on Windows')
    def test_symlink_is_skipped(self):
        alias = self.library / 'Alias.xml'
        alias.symlink_to(self.source)
        self.assertEqual(self.plan(files=[alias]).items[0].status, 'skipped')

    def test_bad_inputs_zero_writes(self):
        bad = [b'', b'\x00\xffcabSectionActive\x00\x01', xml().replace(b'<cab>1</cab>', b'<cab>1</cab><cab>1</cab>', 1),
               xml().replace(b'version="1"', b'version="2"'), xml()[:-20], b'x' * (4*1024*1024+1),
               b'<!DOCTYPE x [<!ENTITY y SYSTEM "file:///secret">]><x>&y;</x>',
               b'<x>' * 70 + b'</x>' * 70]
        for data in bad:
            with self.subTest(size=len(data)):
                self.source.write_bytes(data)
                p = self.plan()
                self.assertEqual(p.items[0].status, 'skipped')
                apply(p)
                self.assertEqual(self.source.read_bytes(), data)

    def test_copy_preserves_embedded_name_even_when_filename_differs(self):
        self.source.write_bytes(xml('Different'))
        plan = self.plan('copy')
        self.assertEqual(plan.items[0].status, 'ready')
        self.assertEqual(apply(plan)[0].status, 'changed')
        self.assertIn(b'<name>Different</name>', plan.items[0].destination.read_bytes())

    def test_ambiguous_registry(self):
        with self.assertRaises(Unsupported):
            Registry((SyntheticXML(), SyntheticXML())).identify(xml(), 'TEST ONLY')

    def test_adapter_outside_span_change_blocked(self):
        class Bad(SyntheticXML):
            def patch(self, data, enabled):
                return super().patch(data, enabled).replace(b'0.21', b'0.99')
        p = preview((Source(self.source, 'TEST ONLY', self.library),), 'disable', registry=Registry((Bad(),)))
        self.assertEqual(p.items[0].status, 'skipped')

    def test_registry_identity_mismatch(self):
        class Bad(SyntheticXML):
            def inspect(self, data):
                return replace(super().inspect(data), version='2')
        with self.assertRaises(Unsupported):
            Registry((Bad(),)).identify(xml(), 'TEST ONLY')

    def test_registry_overlapping_spans(self):
        class Bad(SyntheticXML):
            def inspect(self, data):
                return replace(super().inspect(data), cab_spans=((1, 4), (3, 5)))
        with self.assertRaises(Unsupported):
            Registry((Bad(),)).identify(xml(), 'TEST ONLY')

    def test_stateless_enable_subprocess(self):
        # New Python process with no prior state, importing only the synthetic test adapter.
        import subprocess, sys
        self.source.write_bytes(xml(state=b'0', second=b'0'))
        code = ('from pathlib import Path; from tests.fixtures import SyntheticXML; '
                'from cabdisabler.adapters import Registry; from cabdisabler.engine import Source,preview,apply; '
                f'p=Path({str(self.source)!r}); '
                'r=apply(preview((Source(p,"TEST ONLY",p.parent),),"enable",registry=Registry((SyntheticXML(),)))); '
                'assert r[0].status=="changed"')
        subprocess.run([sys.executable, '-c', code], check=True)
        self.assertEqual(self.source.read_bytes(), xml())
