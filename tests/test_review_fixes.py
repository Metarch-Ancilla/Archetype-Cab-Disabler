from dataclasses import replace
from pathlib import Path
import os
import tempfile
import threading
import unittest
from unittest.mock import patch
from cabdisabler.engine import Source, preview, apply
from cabdisabler.adapters import Registry
from cabdisabler.discovery import Root, discover
from cabdisabler.files import identity
from tests.fixtures import SyntheticXML, xml


class ReviewFixTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name).resolve()
        self.a=self.root/'Tone.xml';self.b=self.root/'Other.xml'
        self.a.write_bytes(xml());self.b.write_bytes(xml('Other'))
        self.registry=Registry((SyntheticXML(),))

    def plan(self, cancel=None):
        return preview(tuple(Source(p,'TEST ONLY',self.root) for p in (self.a,self.b)),
                       'disable',registry=self.registry,cancel=cancel)

    def test_clearing_cancel_does_not_make_partial_preview_applicable(self):
        event=threading.Event();event.set()
        plan=self.plan(event)
        self.assertFalse(plan.complete)
        event.clear()
        self.assertFalse(apply(plan, backup_path=self.root/'backup.zip'))
        self.assertFalse((self.root/'backup.zip').exists())
        self.assertEqual(self.a.read_bytes(),xml())

    def test_partial_preview_with_ready_items_cannot_write(self):
        plan=replace(self.plan(),complete=False)
        results=apply(plan)
        self.assertTrue(all(r.status=='cancelled' for r in results))
        self.assertEqual(self.a.read_bytes(),xml())

    def test_already_correct_items_do_not_consume_output_memory_budget(self):
        self.a.write_bytes(xml(state=b'0',second=b'0'))
        with patch('cabdisabler.engine.MAX_PLAN_BYTES',len(self.b.read_bytes())):
            plan=self.plan()
        self.assertEqual([i.status for i in plan.items],['already-correct','ready'])
        self.assertIsNone(plan.items[0].output)

    def test_cleanup_failure_does_not_abort_batch(self):
        original=Path.unlink
        def fail_temporary(path,*args,**kwargs):
            if path.name.startswith('.cab-'):
                raise PermissionError('cleanup denied')
            return original(path,*args,**kwargs)
        with patch.object(Path,'unlink',fail_temporary):
            plan=preview(tuple(Source(p,'TEST ONLY',self.root) for p in (self.a,self.b)),
                         'copy',registry=self.registry)
            results=apply(plan)
        self.assertEqual([r.status for r in results],['changed','changed'])
        self.assertTrue(all('cleanup failed' in r.reason for r in results))

    def test_source_change_before_direct_open_is_not_overwritten(self):
        original_open=os.open
        def changing_open(path, flags, *args, **kwargs):
            if path==self.a and flags & os.O_RDWR:
                self.a.write_bytes(xml('Concurrent'))
            return original_open(path,flags,*args,**kwargs)
        plan=self.plan()
        with patch('cabdisabler.files.os.open',side_effect=changing_open):
            result=apply(plan)
        self.assertEqual(result[0].status,'failed')
        self.assertEqual(result[1].status,'changed')
        self.assertEqual(self.a.read_bytes(),xml('Concurrent'))

    def test_inode_zero_does_not_deduplicate_distinct_paths(self):
        from types import SimpleNamespace
        fake=SimpleNamespace(st_dev=1,st_ino=0)
        self.assertNotEqual(identity(self.a,fake),identity(self.b,fake))

    def test_windows_signature_uses_consistent_handle_timestamps(self):
        from cabdisabler.files import signature, _signature
        with self.a.open('rb') as stream:
            expected = _signature(os.fstat(stream.fileno()))
        # A path-based stat may expose a different timestamp after replacement.
        with patch('cabdisabler.files.os.name', 'nt'), patch.object(Path, 'stat', side_effect=AssertionError('incompatible path timestamps')):
            self.assertEqual(signature(self.a), expected)

    def test_overlapping_roots_keep_folder_identity(self):
        folder=self.root/'Presets'/'Inferred'/'Factory';folder.mkdir(parents=True)
        (folder/'Tone.xml').write_bytes(b'unknown')
        found=discover((Root(self.root/'Presets','Presets'),
                        Root(self.root/'Presets'/'Inferred','Presets','Explicit Archetype')))
        self.assertEqual(len(found.libraries),1)
        self.assertEqual(found.libraries[0].product,'Inferred')

    def test_root_permission_error_is_reported(self):
        with patch('cabdisabler.discovery.Path.exists',side_effect=PermissionError('denied')):
            found=discover((Root(self.root,'Presets'),))
        self.assertTrue(any('denied' in issue for issue in found.issues))
