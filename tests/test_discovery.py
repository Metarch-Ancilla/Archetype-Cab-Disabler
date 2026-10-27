from pathlib import Path
import os
import tempfile
import threading
import unittest
from unittest.mock import patch
from cabdisabler.discovery import Root, discover, default_roots


class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.presets = self.root / 'Presets'
        for product in ('Archetype Test', 'Archetype Test X'):
            for category in ('Factory', 'Artists', 'User', 'Other'):
                folder = self.presets / product / category
                folder.mkdir(parents=True)
                (folder / 'Tone.xml').write_bytes(b'unknown')
        self.plugins = self.root / 'Plugins'
        self.plugins.mkdir()
        (self.plugins / 'Archetype Test.vst3').mkdir()
        (self.plugins / 'Archetype Test.component').mkdir()
        self.roots = (Root(self.presets, 'Presets'), Root(self.plugins, 'Plugins'))

    def test_original_x_folder_names_without_installation_scans(self):
        result = discover(self.roots)
        self.assertEqual(len(result.libraries), 2)
        original, x = result.libraries
        self.assertEqual(len(original.sources), 4)
        self.assertEqual({s.category for s in original.sources}, {'Factory', 'Artist', 'User', 'Unknown'})

    def test_nested_presets_directory_uses_parent_plugin_name(self):
        folder = self.root / 'Neural DSP' / 'Archetype New' / 'Presets' / 'Neural DSP'
        folder.mkdir(parents=True);(folder / 'Tone.xml').write_bytes(b'unknown')
        found = discover((Root(self.root / 'Neural DSP'),))
        self.assertEqual(found.libraries[0].product, 'Archetype New')
        self.assertEqual(found.libraries[0].path, folder.parent)

    def test_generated_copy_folder_is_not_scanned(self):
        output = self.presets / 'Archetype Test' / 'Cab Disabled' / 'User'
        output.mkdir(parents=True);(output / 'Copied CD.xml').write_bytes(b'unknown')
        found = discover(self.roots)
        self.assertEqual(sum(len(l.sources) for l in found.libraries), 8)

    def test_nested_named_folders_do_not_split_plugin_library(self):
        folder = self.presets / 'Archetype Test' / 'Custom' / 'Artist' / 'Nested'
        folder.mkdir(parents=True);(folder / 'Extra.xml').write_bytes(b'unknown')
        result = discover(self.roots)
        self.assertEqual(len(result.libraries), 2)
        self.assertEqual(result.libraries[0].path, self.presets / 'Archetype Test')
        self.assertEqual(len(result.libraries[0].sources), 5)

    def test_custom_product_root(self):
        result = discover((Root(self.presets / 'Archetype Test X', 'Presets', 'Explicit X'),))
        self.assertEqual(len(result.libraries), 1)
        self.assertEqual(result.libraries[0].product, 'Archetype Test X')

    def test_entry_limit_reported(self):
        result = discover(self.roots, max_entries=1)
        self.assertTrue(any('limit' in issue for issue in result.issues))

    def test_depth_limit_reported(self):
        result = discover(self.roots, max_depth=0)
        self.assertTrue(any('depth' in issue for issue in result.issues))

    def test_cancel(self):
        event = threading.Event()
        event.set()
        result = discover(self.roots, cancel=event)
        self.assertTrue(any('cancelled' in issue for issue in result.issues))

    def test_missing_location_no_crash(self):
        result = discover((Root(self.root / 'missing', 'Presets'),))
        self.assertFalse(result.libraries)

    def test_permission_error_reported(self):
        with patch('cabdisabler.discovery.os.scandir', side_effect=PermissionError('denied')):
            result = discover(self.roots)
        self.assertTrue(any('denied' in issue for issue in result.issues))

    def test_plugin_does_not_imply_library(self):
        result = discover((Root(self.plugins, 'Plugins'),))
        self.assertFalse(result.libraries)

    def test_non_preset_roots_are_never_opened(self):
        (self.plugins / 'Mantra.vst3').mkdir()
        with patch('cabdisabler.discovery.os.scandir', side_effect=AssertionError('plugin scan')):
            result = discover((Root(self.plugins, 'Plugins'),))
        self.assertFalse(result.libraries)

    def test_multiple_roots_dedup_physical_presets(self):
        result = discover(self.roots + self.roots)
        self.assertEqual(sum(len(l.sources) for l in result.libraries), 8)

    @unittest.skipIf(os.name == 'nt', 'Symlink permissions vary on Windows')
    def test_symlink_loop_ignored(self):
        (self.presets / 'loop').symlink_to(self.presets, target_is_directory=True)
        result = discover(self.roots)
        self.assertEqual(sum(len(l.sources) for l in result.libraries), 8)

    def test_windows_roots_use_environment(self):
        roots = default_roots('win32', {'PROGRAMDATA': '/relocated/data', 'ProgramFiles': '/relocated/apps'})
        self.assertEqual(roots[0].path, Path('/relocated/data/Neural DSP'))
        self.assertEqual(len(roots), 1)
        self.assertTrue(all(r.kind == 'Presets' for r in roots))
        self.assertFalse(default_roots('win32', {}))

    def test_macos_per_user_roots(self):
        roots = default_roots('darwin', {}, Path('/custom/home'))
        self.assertEqual([r.path for r in roots], [Path('/Library/Audio/Presets/Neural DSP'), Path('/custom/home/Library/Audio/Presets/Neural DSP')])
