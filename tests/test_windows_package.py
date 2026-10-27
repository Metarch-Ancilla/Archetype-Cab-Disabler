"""Windows downloads contain only the executable, never build intermediates."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('package_windows', Path(__file__).resolve().parents[1] / 'scripts/package_windows.py')
package = importlib.util.module_from_spec(spec);spec.loader.exec_module(package)


class WindowsPackageTests(unittest.TestCase):
    def test_single_executable_ignores_stale_runtime_and_build_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'input';root.mkdir()
            for relative in ('Archetype Cab Disabler.exe', 'CabAuthorization.dll', 'CabPrivilege.exe',
                             'backend/CabBackend.exe', 'LICENSES.txt', 'README.md', 'build-info.json'):
                path = root / relative;path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'synthetic packaging fixture')
            archive = package.package_windows(root, Path(directory) / 'synthetic.zip')
            with zipfile.ZipFile(archive) as stream:
                self.assertEqual(stream.namelist(), ['Archetype Cab Disabler.exe'])
                self.assertEqual(stream.read('Archetype Cab Disabler.exe'), b'synthetic packaging fixture')

    def test_missing_executable_fails_before_archive_creation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory);archive = root / 'missing.zip'
            with self.assertRaises(ValueError): package.package_windows(root, archive)
            self.assertFalse(archive.exists())
