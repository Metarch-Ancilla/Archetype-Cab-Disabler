"""Distribution notices must preserve the original texts and source attributions."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LicenseNoticeTests(unittest.TestCase):
    def test_headers_reference_current_application_version(self):
        version = re.search(r'^#define CAB_VERSION "([^\"]+)"',
                            (ROOT / 'shared/version.h').read_text(), re.MULTILINE).group(1)
        for platform in ('windows', 'macos'):
            self.assertIn('Version: ' + version,
                          (ROOT / f'licenses/{platform}-LICENSES.txt').read_text().splitlines()[:3])

    def test_both_platforms_preserve_application_expat_and_siphash_notices(self):
        for platform in ('windows', 'macos'):
            with self.subTest(platform=platform):
                text = (ROOT / f'licenses/{platform}-LICENSES.txt').read_text()
                for original in ('LICENSE', 'third_party/expat/COPYING'):
                    self.assertIn((ROOT / original).read_text().rstrip('\n'), text)
                for path in (ROOT / 'third_party/expat').iterdir():
                    if path.suffix in ('.c', '.h'):
                        for line in path.read_text().splitlines():
                            if re.match(r'\s*Copyright \(c\)', line):
                                self.assertIn(line.strip(), text, path.name)
                self.assertIn('CC0 1.0 Universal', text)
                self.assertIn('Derived by William Ahern', text)
                self.assertIn('Daniel J. Berstein', text)

    def test_runtime_specific_exceptions_and_full_licenses_remain(self):
        windows = (ROOT / 'licenses/windows-LICENSES.txt').read_text()
        self.assertIn('GCC RUNTIME LIBRARY EXCEPTION', windows)
        self.assertIn('Version 3.1, 31 March 2009', windows)
        self.assertIn('Version 3, 29 June 2007', windows)
        self.assertIn('License: BSD-3-clause-Lockless', windows)
        self.assertIn('ZPL-2.1', windows)
        macos = (ROOT / 'licenses/macos-LICENSES.txt').read_text()
        self.assertIn((ROOT / 'licenses/inputs/libcxx-15.0.6-LICENSE.txt').read_text().rstrip('\n'), macos)
        self.assertNotIn('GCC RUNTIME LIBRARY EXCEPTION', macos)
