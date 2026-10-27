"""Build provenance and cache invalidation without compiling an application."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import build_common
import native_build


class NativeBuildHelpersTests(unittest.TestCase):
    def test_license_build_gate_rejects_stale_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'licenses').mkdir()
            notice=root/'licenses/windows-LICENSES.txt'
            notice.write_text('Application notices\nVersion: 1.2.3\n')
            with patch.object(build_common,'ROOT',root):
                self.assertEqual(build_common.license_file('windows','1.2.3'),notice)
                with self.assertRaisesRegex(ValueError,'version mismatch'):
                    build_common.license_file('windows','1.2.4')

    def test_shared_protocol_and_codec_changes_invalidate_backend_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            tools = Path(directory)
            for name in ('download-inputs.json', 'debian-inputs.json'): (tools / name).write_text('[]')
            original = Path.read_bytes
            baseline = native_build.fingerprint(tools, 'windows')
            self.assertNotEqual(baseline, native_build.fingerprint(tools, 'arm64'))
            for relative in ('shared/text.hpp', 'shared/version.h', 'frontends/shared/wire.hpp'):
                changed = native_build.ROOT / relative
                with self.subTest(header=relative), patch.object(Path, 'read_bytes', lambda p: original(p) + (b'\n// changed' if p == changed else b'')):
                    self.assertNotEqual(baseline, native_build.fingerprint(tools, 'windows'))

    def test_windows_resource_edits_reuse_mac_backend_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            tools = Path(directory)
            for name in ('download-inputs.json', 'debian-inputs.json'): (tools / name).write_text('[]')
            original = Path.read_bytes
            before = {target: native_build.fingerprint(tools, target) for target in ('windows', 'arm64')}
            resource = native_build.ROOT / 'frontends/windows/app.manifest'
            with patch.object(Path, 'read_bytes', lambda p: original(p) + (b'<!-- changed -->' if p == resource else b'')):
                self.assertNotEqual(before['windows'], native_build.fingerprint(tools, 'windows'))
                self.assertEqual(before['arm64'], native_build.fingerprint(tools, 'arm64'))

    def test_failed_cache_publication_retains_previous_complete_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = root / 'source'; cache = root / 'cache/backend'
            source.write_bytes(b'new complete backend'); cache.parent.mkdir(); cache.write_bytes(b'previous complete backend')
            def partial_copy(_, destination):
                Path(destination).write_bytes(b'partial'); raise OSError('simulated interrupted copy')
            with patch.object(native_build.shutil, 'copy2', partial_copy), self.assertRaises(OSError): native_build.publish_cache(source, cache)
            self.assertEqual(cache.read_bytes(), b'previous complete backend')
            self.assertEqual(list(cache.parent.iterdir()), [cache])
            native_build.publish_cache(source, cache)
            self.assertEqual(cache.read_bytes(), source.read_bytes())

    def test_streamed_artifact_digest_and_canonical_version(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / 'test.zip'; archive.write_bytes(b'generated archive bytes')
            self.assertEqual(build_common.artifact_record(archive), dict(artifact=archive.name, bytes=archive.stat().st_size, sha256=hashlib.sha256(archive.read_bytes()).hexdigest()))
        self.assertRegex(build_common.VERSION, r'^\d+\.\d+\.\d+$')
        header = (build_common.ROOT / 'shared/version.h').read_text()
        self.assertIn('#define CAB_FILE_VERSION ' + build_common.VERSION.replace('.', ',') + ',0', header)
        self.assertIn('#define CAB_RESOURCE_VERSION \"' + build_common.VERSION + '.0\"', header)
        resources = (build_common.ROOT / 'frontends/windows/version.rcinc').read_text()
        self.assertIn('CAB_VERSION', resources)
        self.assertIn(f'version="{build_common.VERSION}.0"', (build_common.ROOT / 'frontends/windows/app.manifest').read_text())
