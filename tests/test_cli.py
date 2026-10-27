import contextlib
import io
import tempfile
from pathlib import Path
import unittest
from cabdisabler.cli import main


class CLITests(unittest.TestCase):
    def run_cli(self, args):
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            code = main(args)
        return code, stream.getvalue()

    def test_provisional_adapter_list(self):
        import json
        code, output = self.run_cli(['adapters'])
        self.assertEqual(code, 0)
        self.assertEqual({a['product'] for a in json.loads(output)},
                         {'Binary ValueTree presets', 'Text XML presets'})

    def test_inspect_missing_returns_failure(self):
        code, output = self.run_cli(['inspect', '/does-not-exist.xml'])
        self.assertEqual(code, 1)
        self.assertIn('error', output)

    def test_unknown_no_write_apply_failure_exit(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'Tone.xml'
            p.write_bytes(b'cabSectionActive\x00\x01')
            code, output = self.run_cli(['edit', 'disable', str(p), '--product', 'Archetype Test',
                                        '--library', d, '--apply'])
            self.assertEqual(code, 1)
            self.assertIn('skipped', output)
            self.assertEqual(p.read_bytes(), b'cabSectionActive\x00\x01')
