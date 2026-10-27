"""Windows provider lifetime/UAC results and conversion to Python-owned handles."""
import ctypes
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
from cabdisabler.authorization import AuthorizationCancelled, provider_factory
from cabdisabler.windows_authorization import DirectAuthorization, native_paths, available


class WindowsAuthorizationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory();self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.path = self.root / '音 Guitar.xml';self.path.write_bytes(b'<x cabActive="1"/>')
        self.api = Mock()
        self.api.cab_windows_authorize.return_value = 123
        self.provider = DirectAuthorization()

    def authorize(self):
        with patch('ctypes.CDLL', return_value=self.api):
            self.provider.authorize((self.path,))

    def test_packaged_paths_beside_frontend_not_backend(self):
        with patch('cabdisabler.windows_authorization.sys.executable', str(self.root / 'backend/CabBackend.exe')):
            self.assertEqual(native_paths(), (self.root / 'CabAuthorization.dll', self.root / 'Archetype Cab Disabler.exe'))

    def test_packaged_provider_needs_main_exe_not_separate_helper(self):
        dll = self.root / 'CabAuthorization.dll'
        exe = self.root / 'Archetype Cab Disabler.exe'
        dll.touch();exe.touch()
        with patch('cabdisabler.windows_authorization.sys.platform', 'win32'), \
             patch('cabdisabler.windows_authorization.native_paths', return_value=(dll, exe)):
            self.assertTrue(available())
            self.assertFalse((self.root / 'CabPrivilege.exe').exists())
            exe.unlink()
            self.assertFalse(available())

    def test_authorization_targets_main_executable(self):
        paths = (self.root / 'CabAuthorization.dll', self.root / 'Archetype Cab Disabler.exe')
        with patch('cabdisabler.windows_authorization.native_paths', return_value=paths):
            self.authorize()
        self.assertEqual(self.api.cab_windows_authorize.call_args.args[0], str(paths[1]))
        self.provider.close()

    def test_uac_cancellation_leaves_original_untouched(self):
        def decline(helper, paths, count, status):
            self.assertEqual(count, 1)
            self.assertEqual(paths[0], str(self.path))
            ctypes.cast(status, ctypes.POINTER(ctypes.c_uint32)).contents.value = 1223
            return None
        self.api.cab_windows_authorize.side_effect = decline
        with self.assertRaises(AuthorizationCancelled):self.authorize()
        self.assertEqual(self.path.read_bytes(), b'<x cabActive="1"/>')
        self.api.cab_windows_authorized_open.assert_not_called()

    def test_uac_launch_failure_has_actionable_error(self):
        def fail(helper, paths, count, status):
            ctypes.cast(status, ctypes.POINTER(ctypes.c_uint32)).contents.value = 5
            return None
        self.api.cab_windows_authorize.side_effect = fail
        with self.assertRaisesRegex(PermissionError, 'Windows error 5'):self.authorize()

    def test_batch_closes_once_and_forgets_approved_files(self):
        self.authorize();self.provider.close();self.provider.close()
        self.api.cab_windows_authorization_close.assert_called_once_with(123)
        self.assertFalse(self.provider.approved)

    def test_unselected_file_never_reaches_elevated_broker(self):
        self.authorize()
        with self.assertRaisesRegex(PermissionError, 'not selected'):
            self.provider.open(self.root / 'Unselected.xml', os.O_RDWR)
        self.api.cab_windows_authorized_open.assert_not_called()
        self.provider.close()

    def test_writable_file_opens_normally_after_authorization(self):
        self.authorize()
        fd = self.provider.open(self.path, os.O_RDWR)
        with os.fdopen(fd, 'rb') as stream:self.assertEqual(stream.read(), self.path.read_bytes())
        self.api.cab_windows_authorized_open.assert_not_called()
        self.provider.close()

    def test_elevated_handle_is_adopted_by_python(self):
        self.authorize();self.api.cab_windows_authorized_open.return_value = 987
        fd = os.open(self.path, os.O_RDWR)
        runtime = SimpleNamespace(open_osfhandle=Mock(return_value=fd))
        with patch('cabdisabler.windows_authorization.os.open', side_effect=PermissionError), \
             patch.dict(sys.modules, {'msvcrt': runtime}):
            received = self.provider.open(self.path, os.O_RDWR)
        runtime.open_osfhandle.assert_called_once_with(987, os.O_RDWR)
        self.api.cab_windows_authorized_open.assert_called_once_with(123, str(self.path))
        with os.fdopen(received, 'rb') as stream:self.assertEqual(stream.read(), self.path.read_bytes())
        self.provider.close()

    def test_failed_handle_adoption_closes_remote_handle(self):
        self.authorize();self.api.cab_windows_authorized_open.return_value = 987
        kernel = Mock();runtime = SimpleNamespace(open_osfhandle=Mock(side_effect=OSError('CRT failed')))
        with patch('cabdisabler.windows_authorization.os.open', side_effect=PermissionError), \
             patch.dict(sys.modules, {'msvcrt': runtime}), patch('ctypes.WinDLL', create=True, return_value=kernel):
            with self.assertRaisesRegex(OSError, 'CRT failed'):self.provider.open(self.path, os.O_RDWR)
        kernel.CloseHandle.assert_called_once_with(987)
        self.provider.close()

    def test_factory_selects_platform_provider(self):
        with patch('cabdisabler.authorization.sys.platform', 'win32'), \
             patch('cabdisabler.windows_authorization.available', return_value=True):
            self.assertIs(provider_factory(), DirectAuthorization)
        with patch('cabdisabler.authorization.sys.platform', 'win32'), \
             patch('cabdisabler.windows_authorization.available', return_value=False):
            self.assertIsNone(provider_factory())
