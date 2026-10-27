"""Existing-inode writing, interruption reporting and exact output verification."""
from pathlib import Path
import os
import tempfile
import unittest
from unittest.mock import patch
from cabdisabler.files import digest, signature, write_original
from cabdisabler.errors import Unsupported


class DirectWriteTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'Tone.xml'
        self.before = b'<preset cabActive="false" other="unchanged"/>'
        self.path.write_bytes(self.before)
        self.snap = signature(self.path)

    def test_length_changes_and_hardlink_identity(self):
        alias = self.path.with_name('Alias.xml')
        os.link(self.path, alias)
        for output in (self.before.replace(b'false', b'true'), self.before):
            before = self.path.read_bytes()
            snap = signature(self.path)
            write_original(self.path, output, snap, digest(before))
            self.assertEqual(self.path.read_bytes(), output)
            self.assertEqual(alias.read_bytes(), output)
            self.assertEqual(signature(self.path)[:2], self.snap[:2])

    def test_wrong_reviewed_bytes_do_not_write(self):
        with self.assertRaises(Unsupported):
            write_original(self.path, b'changed', self.snap, digest(b'other content'))
        self.assertEqual(self.path.read_bytes(), self.before)

    def test_flush_failure_reports_possible_partial_write(self):
        with patch('cabdisabler.files.os.fsync', side_effect=OSError('disk error')):
            with self.assertRaisesRegex(Unsupported, 'partially modified.*backup'):
                write_original(self.path, self.before.replace(b'false', b'true'), self.snap, digest(self.before))

    def test_readback_detects_corrupted_output(self):
        def corrupt_after_flush(_fd):
            with self.path.open('r+b') as stream:
                stream.write(b'CORRUPTED')
        with patch('cabdisabler.files.os.fsync', side_effect=corrupt_after_flush):
            with self.assertRaisesRegex(Unsupported, 'read-back verification failed'):
                write_original(self.path, self.before.replace(b'false', b'true'), self.snap, digest(self.before))

    @unittest.skipUnless(hasattr(os, 'setxattr'), 'Filesystem xattrs unavailable')
    def test_existing_security_metadata_is_not_recreated(self):
        try:
            os.setxattr(self.path, 'user.cab-test', b'preserved')
        except OSError as exc:
            self.skipTest(str(exc))
        self.path.chmod(0o640)
        before = self.path.stat()
        with patch('cabdisabler.files.os.chown', side_effect=AssertionError('No ownership changes')), \
             patch('cabdisabler.files.os.replace', side_effect=AssertionError('No inode replacement')):
            write_original(self.path, self.before.replace(b'false', b'true'), signature(self.path), digest(self.before))
        after = self.path.stat()
        self.assertEqual((after.st_ino, after.st_uid, after.st_gid, after.st_mode),
                         (before.st_ino, before.st_uid, before.st_gid, before.st_mode))
        self.assertEqual(os.getxattr(self.path, 'user.cab-test'), b'preserved')
