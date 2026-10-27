"""Native bundle seals/archive integrity without building an application."""
import hashlib
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
try:
    from cross.verify_macos import verify_app
    from cross_build_macos import archive_app
except ModuleNotFoundError as error:
    if error.name != 'macholib':
        raise
    raise unittest.SkipTest('macOS bundle checks require requirements-build.txt')


class MacBundleTests(unittest.TestCase):
    def test_native_layout_and_resource_tamper_detection(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / 'Example.app';root = app / 'Contents'
            (root / '_CodeSignature').mkdir(parents=True)
            (root / 'MacOS').mkdir();(root / 'Resources').mkdir()
            executable = root / 'MacOS/CustomName';executable.write_bytes(b'generated')
            notice = root / 'Resources/LICENSES.txt';notice.write_bytes(b'legal notice')
            (root / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable': 'CustomName'}))
            (root / '_CodeSignature/CodeResources').write_bytes(plistlib.dumps({'files2': {
                'Resources/LICENSES.txt': {'hash2': hashlib.sha256(notice.read_bytes()).digest()}}}))
            with patch('cross.verify_macos.verify_binary', return_value=[b'digest']) as verifier:
                self.assertEqual(verify_app(app, [executable]), 1)
                self.assertEqual(verifier.call_args.args[0], executable)
                self.assertEqual(verifier.call_args.args[1][1], (root / 'Info.plist').read_bytes())
                notice.write_bytes(b'tampered notice')
                with self.assertRaisesRegex(ValueError, 'Resource hash mismatch'):
                    verify_app(app, [executable])

    def test_archive_profiles_preserve_content_permissions_and_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / 'Example.app';app.mkdir()
            binary = app / 'program';binary.write_bytes(b'generated native file');binary.chmod(0o755)
            notice = app / 'LICENSES.txt';notice.write_bytes(b'legal notice\n' * 200)
            (app / 'alias').symlink_to('program')
            for full in (False, True):
                archive = Path(directory) / f'{full}.zip';archive_app(app, archive, full=full)
                with zipfile.ZipFile(archive) as stream:
                    self.assertEqual(stream.read('Example.app/LICENSES.txt'), notice.read_bytes())
                    self.assertEqual(stream.read('Example.app/alias'), b'program')
                    self.assertEqual(stream.getinfo('Example.app/program').external_attr >> 16 & 0o777, 0o755)
