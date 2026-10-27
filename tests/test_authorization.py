"""Authorisation is scoped to Direct Edit and follows verified normal-user backup."""
from pathlib import Path
import errno
import os
import tempfile
import unittest
from unittest.mock import Mock, patch
from cabdisabler.adapters import Registry
from cabdisabler.backend.session import Session
from cabdisabler.engine import Source, preview, apply
from cabdisabler.macos_authorization import DirectAuthorization, AuthorizationCancelled
from tests.fixtures import SyntheticXML, xml


class AuthorizationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.source = self.root / 'Tone.xml'
        self.source.write_bytes(xml())
        self.registry = Registry((SyntheticXML(),))
        self.sources = (Source(self.source, 'TEST ONLY', self.root),)
        self.backup = self.root / 'backup.zip'
        self.provider = Mock()
        self.provider.open.side_effect = os.open
        self.probe = patch('cabdisabler.authorization.requires_authorization', return_value=True)
        self.probe.start();self.addCleanup(self.probe.stop)

    def plan(self, operation='disable', **kwargs):
        return preview(self.sources, operation, registry=self.registry, **kwargs)

    def test_verified_backup_precedes_prompt_and_write(self):
        import json, zipfile
        def authorize(paths):
            self.assertEqual(self.source.read_bytes(), xml())
            with zipfile.ZipFile(self.backup) as archive:
                manifest = json.loads(archive.read('backup-manifest.json'))
                self.assertEqual(archive.read(manifest['files'][0]['entry']), xml())
        self.provider.authorize.side_effect = authorize
        result = apply(self.plan(), backup_path=self.backup, direct_authorization=self.provider)
        self.assertEqual(result[0].status, 'changed')
        self.provider.authorize.assert_called_once_with((self.source,))
        self.provider.close.assert_called_once_with()
        self.assertEqual(SyntheticXML().inspect(self.source.read_bytes()).enabled, (False, False))

    def test_backup_verification_failure_never_prompts_or_edits(self):
        with patch('cabdisabler.backup.zipfile.ZipFile.read', return_value=b'corrupt manifest'):
            result = apply(self.plan(), backup_path=self.backup, direct_authorization=self.provider)
        self.assertEqual(result[0].status, 'failed')
        self.provider.authorize.assert_not_called()
        self.provider.open.assert_not_called()
        self.assertEqual(self.source.read_bytes(), xml())

    def test_decline_stops_entire_batch_without_writes(self):
        other = self.root / 'Other.xml';other.write_bytes(xml('Other'))
        plan = preview((*self.sources, Source(other, 'TEST ONLY', self.root)), registry=self.registry)
        self.provider.authorize.side_effect = AuthorizationCancelled('Administrator authorisation cancelled; no presets edited')
        result = apply(plan, direct_authorization=self.provider)
        self.assertEqual([r.status for r in result], ['cancelled', 'cancelled'])
        self.provider.open.assert_not_called()
        self.assertEqual(self.source.read_bytes(), xml())
        self.assertEqual(other.read_bytes(), xml('Other'))

    def test_authorization_failure_keeps_verified_backup_and_original(self):
        self.provider.authorize.side_effect = PermissionError('Authorisation failed')
        result = apply(self.plan(), backup_path=self.backup, direct_authorization=self.provider)
        self.assertEqual(result[0].status, 'failed')
        self.assertTrue(self.backup.exists())
        self.assertEqual(self.source.read_bytes(), xml())

    def test_copy_never_prompts(self):
        self.assertEqual(apply(self.plan('copy'), direct_authorization=self.provider)[0].status, 'changed')
        self.provider.authorize.assert_not_called()
        self.provider.open.assert_not_called()

    def test_already_correct_never_prompts(self):
        result = apply(self.plan('enable'), direct_authorization=self.provider)
        self.assertEqual(result[0].status, 'already-correct')
        self.provider.authorize.assert_not_called()

    def test_permission_override_is_explicit_and_direct_only(self):
        with patch('cabdisabler.engine.os.access', return_value=False):
            self.assertEqual(self.plan().items[0].status, 'failed')
            self.assertEqual(self.plan(allow_elevation=True).items[0].status, 'ready')
            self.assertEqual(self.plan('copy', allow_elevation=True).items[0].status, 'failed')

    def test_privileged_handle_cannot_bypass_reviewed_bytes(self):
        plan = self.plan()
        def authorize(paths):
            self.source.write_bytes(xml('Concurrent'))
        self.provider.authorize.side_effect = authorize
        self.assertEqual(apply(plan, direct_authorization=self.provider)[0].status, 'failed')
        self.assertEqual(self.source.read_bytes(), xml('Concurrent'))
        self.provider.close.assert_called_once()

    def test_description_contains_explainer(self):
        session = Session();session.page = 3
        direct = session.view()['controls'][1]['options'][0]
        self.assertIn('Direct edit may require elevated permissions.', direct[-1])

    def test_privileged_open_restricted_to_selected_files_anywhere(self):
        provider = DirectAuthorization();provider.handle = 123;provider.library = Mock()
        provider.approved = frozenset((self.source,))
        provider.library.cab_authorized_open.return_value = 7
        with patch('cabdisabler.macos_authorization.os.open', side_effect=PermissionError):
            self.assertEqual(provider.open(self.source, os.O_RDWR), 7)
            with self.assertRaisesRegex(PermissionError, 'not selected'):
                provider.open(self.root / 'Unselected.xml', os.O_RDWR)
        provider.library.cab_authorized_open.assert_called_once_with(123, os.fsencode(self.source))

    def test_writable_direct_edit_never_prompts(self):
        with patch('cabdisabler.authorization.requires_authorization', return_value=False):
            result = apply(self.plan(), direct_authorization=self.provider)
        self.assertEqual(result[0].status, 'changed')
        self.provider.authorize.assert_not_called()
        self.provider.open.assert_not_called()

    def test_access_probe_detects_any_selected_file_without_writing(self):
        self.probe.stop()
        from cabdisabler.authorization import requires_authorization
        self.assertFalse(requires_authorization((self.source,)))
        other = self.root / 'Other.xml'
        original_open = os.open
        def permission(path, flags):
            if path == other:raise PermissionError('protected')
            return original_open(path, flags)
        with patch('cabdisabler.macos_authorization.os.open', side_effect=permission):
            self.assertTrue(requires_authorization((self.source, other)))
        self.assertEqual(self.source.read_bytes(), xml())

    def test_ordinary_browsed_file_uses_normal_permissions(self):
        provider = DirectAuthorization();provider.approved = frozenset((self.source,))
        fd = provider.open(self.source, os.O_RDWR)
        os.close(fd)

    def test_authorization_api_cancellation_and_close(self):
        import ctypes
        api = Mock()
        def decline(helper, paths, count, status):
            self.assertEqual(count, 1)
            self.assertEqual(paths[0], os.fsencode(self.source))
            ctypes.cast(status, ctypes.POINTER(ctypes.c_int)).contents.value = errno.ECANCELED
            return None
        api.cab_authorize.side_effect = decline
        with patch('ctypes.CDLL', return_value=api):
            provider = DirectAuthorization()
            with self.assertRaises(AuthorizationCancelled):provider.authorize((self.source,))
        api.cab_authorization_close.assert_not_called()
        provider.handle = 123;provider.close();provider.close()
        api.cab_authorization_close.assert_called_once_with(123)
