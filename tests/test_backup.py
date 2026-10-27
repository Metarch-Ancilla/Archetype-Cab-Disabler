import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch
import zipfile
from cabdisabler.backup import create_backup, default_backup_path
from cabdisabler.engine import apply, preview, Source
from cabdisabler.adapters import Registry
from tests.fixtures import SyntheticXML, xml


class BackupTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.source = self.root / 'presets' / 'Factory' / 'Tone.xml'
        self.source.parent.mkdir(parents=True)
        self.source.write_bytes(xml())
        self.destination = self.root / 'backups' / 'originals.zip'

    def plan(self, paths=None):
        return preview(tuple(Source(p, 'TEST ONLY', self.root / 'presets') for p in paths or [self.source]),
                       'disable', registry=Registry((SyntheticXML(),)))

    def test_default_is_desktop_zip(self):
        with patch('cabdisabler.backup.desktop_directory', return_value=self.root / 'Desktop'):
            self.assertEqual(default_backup_path().parent, self.root / 'Desktop')
            self.assertEqual(default_backup_path().suffix, '.zip')

    def test_redirected_windows_desktop_guid_and_com_cleanup(self):
        import ctypes
        import uuid
        from types import SimpleNamespace
        from unittest.mock import Mock
        from cabdisabler.backup import desktop_directory
        redirected = self.root / 'Redirected Desktop'
        def get_folder(guid, flags, token, output):
            actual = bytes(ctypes.cast(guid, ctypes.POINTER(ctypes.c_ubyte * 16)).contents)
            self.assertEqual(actual, uuid.UUID('B4BFCC3A-DB2C-424C-B029-7FE99A87C641').bytes_le)
            self.assertEqual(flags, 0)
            output._obj.value = str(redirected)
            return 0
        shell = SimpleNamespace(SHGetKnownFolderPath=Mock(side_effect=get_folder))
        memory = SimpleNamespace(CoTaskMemFree=Mock())
        with patch('cabdisabler.backup.sys.platform', 'win32'), patch('ctypes.WinDLL', create=True, side_effect=[shell, memory]):
            self.assertEqual(desktop_directory(), redirected.resolve())
        memory.CoTaskMemFree.assert_called_once()

    def test_zip_preserves_originals_and_nested_duplicate_names(self):
        other = self.root / 'presets' / 'User' / 'Tone.xml'
        other.parent.mkdir()
        other.write_bytes(xml('Different'))
        before = {str(p): p.read_bytes() for p in (self.source, other)}
        plan = self.plan([self.source, other])
        self.assertEqual(create_backup(plan, self.destination), self.destination)
        with zipfile.ZipFile(self.destination) as archive:
            manifest = json.loads(archive.read('backup-manifest.json'))
            self.assertEqual(len(manifest['files']), 2)
            self.assertEqual(len({r['entry'] for r in manifest['files']}), 2)
            for record in manifest['files']:
                self.assertEqual(archive.read(record['entry']), before[record['source']])
        self.assertEqual({str(p): p.read_bytes() for p in (self.source, other)}, before)

    def test_readable_date_filename_and_existing_name_collision(self):
        from datetime import datetime
        with patch('cabdisabler.backup.desktop_directory', return_value=self.root), \
             patch('cabdisabler.backup.datetime') as clock:
            clock.now.return_value = datetime(2026, 10, 23, 14, 15, 16, 123456)
            first = default_backup_path()
            self.assertEqual(first.name, 'NeuralDSP presets 2026-10-23.zip')
            first.write_bytes(b'keep')
            second = default_backup_path()
            self.assertEqual(second.name, 'NeuralDSP presets 2026-10-23 (2).zip')
            self.assertEqual(first.read_bytes(), b'keep')

    def test_backup_completes_before_first_edit(self):
        def check_created(path):
            with zipfile.ZipFile(path) as archive:
                self.assertIsNone(archive.testzip())
            self.assertEqual(self.source.read_bytes(), xml())
        result = apply(self.plan(), backup_path=self.destination, backup_created=check_created)
        self.assertEqual(result[0].status, 'changed')
        with zipfile.ZipFile(self.destination) as archive:
            manifest = json.loads(archive.read('backup-manifest.json'))
            self.assertEqual(archive.read(manifest['files'][0]['entry']), xml())

    def test_no_backup_is_optional(self):
        self.assertEqual(apply(self.plan())[0].status, 'changed')
        self.assertFalse(self.destination.exists())

    def test_existing_backup_not_overwritten_and_no_edits(self):
        self.destination.parent.mkdir()
        self.destination.write_bytes(b'existing unrelated archive')
        result = apply(self.plan(), backup_path=self.destination)
        self.assertEqual(result[0].status, 'failed')
        self.assertEqual(self.source.read_bytes(), xml())
        self.assertEqual(self.destination.read_bytes(), b'existing unrelated archive')

    def test_backup_write_failure_stops_all_edits_and_cleans_archive(self):
        with patch('cabdisabler.backup.zipfile.ZipFile.writestr', side_effect=OSError('full disk')):
            result = apply(self.plan(), backup_path=self.destination)
        self.assertEqual(result[0].status, 'failed')
        self.assertEqual(self.source.read_bytes(), xml())
        self.assertFalse(self.destination.exists())

    def test_cancel_during_backup_cleans_zip_and_stops_edits(self):
        event = threading.Event()
        def progress(*_):
            event.set()
        result = apply(self.plan(), event, backup_path=self.destination, backup_progress=progress)
        self.assertEqual(result[0].status, 'cancelled')
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.source.read_bytes(), xml())

    def test_concurrent_source_change_blocks_backup_and_edits(self):
        plan = self.plan()
        self.source.write_bytes(xml('Concurrent'))
        result = apply(plan, backup_path=self.destination)
        self.assertEqual(result[0].status, 'failed')
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.source.read_bytes(), xml('Concurrent'))

    def test_only_files_to_be_changed_are_archived(self):
        correct = self.source.parent / 'Correct.xml'
        correct.write_bytes(xml('Correct', b'0', b'0'))
        unknown = self.source.parent / 'Unknown.xml'
        unknown.write_bytes(b'unknown')
        plan = self.plan([self.source, correct, unknown])
        create_backup(plan, self.destination)
        with zipfile.ZipFile(self.destination) as archive:
            manifest = json.loads(archive.read('backup-manifest.json'))
            self.assertEqual([r['source'] for r in manifest['files']], [str(self.source)])

    def test_no_changes_needs_no_archive(self):
        self.source.write_bytes(xml(state=b'0', second=b'0'))
        self.assertIsNone(create_backup(self.plan(), self.destination))
        self.assertFalse(self.destination.exists())

    def test_zip_cannot_replace_source(self):
        result = apply(self.plan(), backup_path=self.source)
        self.assertEqual(result[0].status, 'failed')
        self.assertEqual(self.source.read_bytes(), xml())

    def test_destination_race_preserves_existing_file(self):
        original = Path.open
        def racing_open(path, mode='r', *args, **kwargs):
            if path == self.destination and mode == 'xb':
                path.write_bytes(b'created by another process')
            return original(path, mode, *args, **kwargs)
        with patch.object(Path, 'open', racing_open):
            result = apply(self.plan(), backup_path=self.destination)
        self.assertEqual(result[0].status, 'failed')
        self.assertEqual(self.destination.read_bytes(), b'created by another process')
        self.assertEqual(self.source.read_bytes(), xml())

    def test_verification_failure_removes_zip_and_prevents_edits(self):
        with patch('cabdisabler.backup.zipfile.ZipFile.read', return_value=b'corrupt manifest'):
            result = apply(self.plan(), backup_path=self.destination)
        self.assertEqual(result[0].status, 'failed')
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.source.read_bytes(), xml())

    def test_payload_hash_failure_stops_entire_batch_before_direct_write(self):
        import io
        other = self.source.parent / 'Other.xml'
        other.write_bytes(xml('Other'))
        plan = self.plan([self.source, other])
        original_open = zipfile.ZipFile.open
        def corrupt_read(archive, name, mode='r', *args, **kwargs):
            if mode == 'r' and str(name).endswith('.xml'):
                return io.BytesIO(b'corrupt archived data')
            return original_open(archive, name, mode, *args, **kwargs)
        with patch('cabdisabler.backup.zipfile.ZipFile.open', corrupt_read), \
             patch('cabdisabler.engine.write_original') as write:
            results = apply(plan, backup_path=self.destination)
        write.assert_not_called()
        self.assertTrue(all(r.status == 'failed' for r in results))
        self.assertEqual(self.source.read_bytes(), xml())
        self.assertEqual(other.read_bytes(), xml('Other'))
        self.assertFalse(self.destination.exists())

    def test_missing_backup_result_stops_direct_write(self):
        with patch('cabdisabler.backup.create_backup', return_value=None), \
             patch('cabdisabler.engine.write_original') as write:
            results = apply(self.plan(), backup_path=self.destination)
        write.assert_not_called()
        self.assertEqual(results[0].status, 'failed')
        self.assertEqual(self.source.read_bytes(), xml())

    def test_one_changed_source_stops_entire_backup_batch(self):
        other = self.source.parent / 'Other.xml'
        other.write_bytes(xml('Other'))
        plan = self.plan([self.source, other])
        def progress(number, _total):
            if number == 1:
                other.write_bytes(xml('Concurrent'))
        results = apply(plan, backup_path=self.destination, backup_progress=progress)
        self.assertTrue(all(r.status == 'failed' for r in results))
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.source.read_bytes(), xml())
        self.assertEqual(other.read_bytes(), xml('Concurrent'))
