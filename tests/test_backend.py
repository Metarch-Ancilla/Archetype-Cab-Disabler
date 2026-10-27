"""Frontend-independent acceptance checks for the actual wizard workflow."""
from pathlib import Path
import subprocess
import queue
import sys
import tempfile
import threading
import unittest
import zipfile

from cabdisabler.backend.server import command, encode, snapshot
from cabdisabler.backend.session import Session
from cabdisabler.discovery import Root


class BackendTests(unittest.TestCase):
    def setUp(self):
        self.events = []
        self.session = Session(lambda *event: self.events.append(event))

    def finish_work(self):
        kind, name, operation = next(e for e in self.events if e[0] == 'work')
        self.events.clear()
        self.assertEqual(kind, 'work')
        self.session.finished(name, operation())

    def review(self, root, backup):
        s = self.session
        s.roots = [Root(root, 'Presets', 'New identity')]
        s.action('scan'); self.finish_work()
        s.action('next', {'plugins': '0'})
        self.assertEqual(s.page, 1)
        s.action('next', {'presets': ','.join(n.id for n in s.nodes if n.source and n.source.path in s.selected_files)})
        self.assertEqual(s.page, 2)  # backup is strictly after selection
        s.action('next', {'backup': str(int(backup)), 'backup_path': str(root / 'backup.zip')})
        s.action('next', {'operation': 'disable'}); self.finish_work()
        self.assertEqual(s.page, 4)

    def test_tree_sorts_loose_presets_after_folders_at_every_level(self):
        from types import SimpleNamespace
        from cabdisabler.backend.selection import tree
        root = Path('/presets')
        paths = ['default.xml', 'aaa.xml', 'Zebra/default.xml',
                 'Zebra/zzz.xml', 'Zebra/ZZ Folder/nested.xml', 'Artist/preset.xml']
        library = SimpleNamespace(path=root, product='Plugin', sources=[
            SimpleNamespace(path=root / name) for name in paths])
        nodes = tree([library], {root})
        self.assertEqual([n.label for n in nodes if n.parent == 0],
                         ['Artist', 'Zebra', 'aaa.xml', 'default.xml'])
        zebra = next(i for i, n in enumerate(nodes) if n.label == 'Zebra')
        self.assertEqual([n.label for n in nodes if n.parent == zebra],
                         ['ZZ Folder', 'default.xml', 'zzz.xml'])
        self.assertTrue(all(n.parent < i for i, n in enumerate(nodes)))

    def test_two_independent_mode_pairs_cover_all_combinations(self):
        s = self.session
        s.page = 3
        for disable, copy, expected in [('1', '0', 'disable'), ('0', '0', 'enable'),
                                         ('1', '1', 'copy'), ('0', '1', 'copy-enable')]:
            s.fields({'disable': disable, 'copy': copy})
            self.assertEqual(s.operation, expected)
            controls = s.view()['controls']
            self.assertEqual([c['kind'] for c in controls], ['pair', 'pair'])
            self.assertTrue(all(sum(o[2] for o in c['options']) == 1 for c in controls))
        with self.assertRaises(ValueError):
            s.fields({'disable': 'invalid'})

    def test_browse_plugin_parent_and_review_relative_hierarchy(self):
        with tempfile.TemporaryDirectory() as directory:
            plugin = Path(directory).resolve() / 'Granophyre'
            path = plugin / 'Presets' / 'Artists' / 'Artist Name' / 'Tone.xml'
            path.parent.mkdir(parents=True)
            original = b'<plugin><PARAM id="cabActiveID" value="1.0"/></plugin>'
            path.write_bytes(original)
            s = self.session
            s.roots = []
            s.action('add', {'path': str(plugin)}); self.finish_work()
            self.assertEqual(s.discovery.libraries[0].path, plugin / 'Presets')
            self.assertEqual(s.view()['title'], 'Select Plugins')
            s.action('next', {'plugins': '0'})
            s.action('next', {'presets': ','.join(n.id for n in s.nodes if n.source)})
            backup = plugin / 'chosen.zip'
            s.action('save_backup', {'path': str(backup), 'backup': '1'})
            self.assertFalse(backup.exists())
            self.assertEqual(s.view()['controls'][0]['kind'], 'radio')
            s.action('next')
            s.action('next', {'disable': '1', 'copy': '0'}); self.finish_work()
            self.assertEqual(s.report, f'Edit mode: Direct edit\nBackup: Yes {backup}\nPresets: 1\n\nGranophyre\n    Artists\n        Artist Name\n            Tone')
            self.assertNotIn(str(path), s.report)
            self.assertNotIn('ready', s.report.lower())
            self.assertEqual(path.read_bytes(), original)
            self.assertFalse(backup.exists())
            s.action('next'); self.finish_work()
            self.assertTrue(backup.exists())
            with zipfile.ZipFile(backup) as archive:
                self.assertIn(original, [archive.read(n) for n in archive.namelist()])

    def test_complete_workflow_backup_and_stateless_enable(self):
        original = b'<new-version><PARAM id="cabActiveID" value="1.0"/><PARAM id="cab1ActiveID" value="0"/></new-version>'
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            path = root / 'Factory' / '音🎸.xml'; path.parent.mkdir(); path.write_bytes(original)
            self.review(root, True)
            self.assertEqual(path.read_bytes(), original)
            self.assertFalse((root / 'backup.zip').exists())
            self.assertNotIn('ack', [c['id'] for c in self.session.view()['controls']])
            self.session.action('next')
            with self.assertRaises(ValueError):
                self.session.action('back')
            self.finish_work()
            self.assertEqual(self.session.page, 5)
            self.assertEqual(self.session.edited, 1)
            self.assertIn('cab1ActiveID" value="0', path.read_text())
            with zipfile.ZipFile(root / 'backup.zip') as archive:
                self.assertIn(original, [archive.read(n) for n in archive.namelist()])
            self.assertEqual(self.session.view()['next_label'], 'Finish')
            with self.assertRaises(ValueError):
                self.session.action('next')
            self.events.clear()
            self.session = Session(lambda *event: self.events.append(event))
            self.review(root, False)
            self.session.action('back', {})
            self.session.action('next', {'operation': 'enable'}); self.finish_work()
            self.session.action('next', {}); self.finish_work()
            self.assertEqual(path.read_bytes(), original)

    def test_results_have_counts_relative_error_tree_and_contextual_skips(self):
        from cabdisabler.engine import Outcome
        from cabdisabler.backend.selection import tree
        from cabdisabler.discovery import Library
        from cabdisabler.engine import Source
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            bad = root / 'Artists' / 'Bad.xml'
            correct = root / 'User' / 'Correct.xml'
            edited = root / 'Artists' / 'Changed.xml'
            s = self.session
            library = Library('Plugin', root, tuple(Source(p, 'Plugin', root) for p in (bad, correct, edited)))
            s.nodes = tree([library], {root})
            for operation, heading in [('disable', 'Already disabled'), ('enable', 'Already enabled'),
                                       ('copy', 'Already disabled'), ('copy-enable', 'Already enabled')]:
                s.operation = operation
                s.finished('apply', (Outcome(bad, 'failed', f'Access denied: {bad}'),
                                     Outcome(correct, 'already-correct', 'Cab section already in requested state'),
                                     Outcome(edited, 'changed', str(edited))))
                view = s.view()
                self.assertEqual(view['next_label'], 'Finish')
                self.assertEqual([c['id'] for c in view['controls']], ['summary', 'details'])
                self.assertEqual(s.result_summary, 'Successful: 2\nFailed: 1')
                self.assertEqual(view['controls'][1]['label'], 'Errors')
                self.assertFalse(s.result_details.startswith('Errors\n'))
                self.assertIn('Plugin\n    Artists\n        Bad — Access denied: Bad', s.result_details)
                self.assertIn(heading + '\nPlugin\n    User\n        Correct', s.result_details)
                self.assertNotIn(str(root), s.result_details)
                self.assertNotIn('Changed.xml', s.result_details)
                self.assertNotIn('already-correct', s.result_details)
            exported = root / 'report.txt'
            with self.assertRaises(ValueError):
                s.action('export', {'path': str(exported)})
            self.assertFalse(exported.exists())

    def test_preset_labels_hide_extensions_and_review_separates_plugins(self):
        from cabdisabler.backend.selection import tree
        from cabdisabler.discovery import Library
        from cabdisabler.engine import Source
        root = Path('/example/Plugin')
        sources = tuple(Source(root / 'Folder.xml' / name, 'Plugin', root)
                        for name in ('First.xml', 'Second.XML'))
        s = self.session
        s.nodes = tree([Library('Plugin', root, sources)], {root})
        s.page = 1
        labels = [o[1] for o in s.view()['controls'][0]['options']]
        self.assertEqual(labels, ['Plugin', 'Folder.xml', 'First', 'Second'])
        lines = s.hierarchy({source.path: '' for source in sources}, separate_plugins=True)
        self.assertEqual(lines, ['Plugin', '    Folder.xml', '        First', '        Second'])
        self.assertEqual([source.path.name for source in sources], ['First.xml', 'Second.XML'])
        other = Source(root.parent / 'Other' / 'Third.xml', 'Other', root.parent / 'Other')
        s.nodes = tree([Library('Plugin', root, sources), Library('Other', other.library, (other,))], {root, other.library})
        lines = s.hierarchy({source.path: '' for source in (*sources, other)}, separate_plugins=True)
        self.assertEqual(lines, ['Plugin', '    Folder.xml', '        First', '        Second', '', 'Other', '    Third'])

    def test_backup_no_rejects_browse_and_location_is_readonly(self):
        s = self.session
        s.page = 2
        s.fields({'backup': '0'})
        controls = s.view()['controls']
        self.assertEqual(controls[0]['label'], 'Create backup?')
        self.assertEqual(controls[1]['kind'], 'readonly')
        self.assertEqual(controls[1]['label'], 'Location')
        before = s.backup_path
        with self.assertRaises(ValueError):
            s.action('save_backup', {'path': '/unselected.zip'})
        self.assertEqual(s.backup_path, before)

    def test_no_backup_notice_is_review_only_and_does_not_block_apply(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve();path = root / 'Factory' / 'Tone.xml'
            path.parent.mkdir();path.write_bytes(b'<x cabActive="1"/>')
            self.review(root, False)
            s = self.session
            view = s.view()
            self.assertEqual(view['controls'][-1]['id'], 'backup_notice')
            self.assertEqual(view['controls'][-1]['value'], 'Backup not enabled!\nBy continuing I assume all responsibility for any loss or damage to presets.')
            self.assertTrue(view['next'])
            s.action('back');s.action('back')
            self.assertEqual(s.page, 2)
            self.assertFalse(any(c['kind'] == 'notice' for c in s.view()['controls']))
            s.action('next', {'backup': '1', 'backup_path': str(root / 'backup.zip')})
            s.action('next', {'operation': 'disable'});self.finish_work()
            self.assertFalse(any(c['kind'] == 'notice' for c in s.view()['controls']))
            s.action('back');s.action('back')
            s.action('next', {'backup': '0'})
            s.action('next', {'operation': 'disable'});self.finish_work()
            self.assertTrue(any(c['kind'] == 'notice' for c in s.view()['controls']))
            s.action('next')
            self.assertFalse(any(c['kind'] == 'notice' for c in s.view()['controls']))
            self.finish_work()
            self.assertEqual(path.read_bytes(), b'<x cabActive="0"/>')
            self.assertFalse((root / 'backup.zip').exists())
            self.assertFalse(any(c['kind'] == 'notice' for c in s.view()['controls']))

    def test_failed_backup_prevents_writes(self):
        original = b'<x cabActive="1"/>'
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve(); path = root / 'Factory' / 'x.xml'
            path.parent.mkdir(); path.write_bytes(original)
            self.review(root, True)
            # Destination race after review must not overwrite or edit.
            (root / 'backup.zip').write_bytes(b'existing')
            self.session.action('next', {}); self.finish_work()
            self.assertEqual(path.read_bytes(), original)
            self.assertEqual((root / 'backup.zip').read_bytes(), b'existing')
            self.assertEqual(self.session.edited, 0)

    def test_backup_cancellation_prevents_writes(self):
        original = b'<x cabActive="1"/>'
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve();path = root / 'Factory' / 'x.xml'
            path.parent.mkdir();path.write_bytes(original)
            self.review(root, True)
            self.session.action('next', {})
            self.session.action('cancel')
            self.finish_work()
            self.assertEqual(path.read_bytes(), original)
            self.assertFalse((root / 'backup.zip').exists())

    def test_reviewed_destination_cannot_change_at_apply(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve();path = root / 'Factory' / 'x.xml'
            path.parent.mkdir();path.write_bytes(b'<x cabActive="1"/>')
            self.review(root, True)
            self.session.action('next', {'ack': '1', 'backup_path': str(root / 'other.zip'), 'backup': '0'})
            self.finish_work()
            self.assertTrue((root / 'backup.zip').exists())
            self.assertFalse((root / 'other.zip').exists())

    def test_wire_unicode_newlines_and_duplicate_rejection(self):
        value = '日本語/🎸\tline\nnext'
        self.assertEqual(command('add\tpath=' + encode(value) + '\n'), ('add', {'path': value}))
        with self.assertRaises(ValueError):
            command('add\tpath=31\tpath=32\n')
        self.session.status = value
        wire = snapshot(self.session.view())
        self.assertIn(encode(value), wire)
        self.assertNotIn(value, wire)
        self.session.page = 1
        self.session.nodes = []
        for invalid in ('-1', '1', 'not-an-index'):
            with self.assertRaises(ValueError):
                self.session.fields({'presets': invalid})
        self.session.page = 2
        with self.assertRaises(ValueError):
            self.session.fields({'backup': 'invalid'})
        self.assertTrue(self.session.backup)

    def test_headless_process_handshake_and_clean_exit(self):
        process = subprocess.run([sys.executable, '-m', 'cabdisabler.backend.server'],
            input='quit\n', text=True, capture_output=True, timeout=10)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertTrue(process.stdout.startswith('HELLO\t2\nSTATE\t'))
        self.assertIn('\nEND\n', process.stdout)
        self.assertEqual(process.stderr, '')

    def test_user_and_individual_presets_are_explicitly_selectable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            for folder in ('Factory', 'Artist', 'User'):
                path = root / folder / 'Nested' / 'Tone.xml'
                path.parent.mkdir(parents=True);path.write_bytes(b'<x cabActive="1"/>')
            s = self.session;s.roots = [Root(root, product=root.name)]
            s.action('scan');self.finish_work();s.action('next', {'plugins': '0'})
            user = next(n for n in s.nodes if n.source and n.source.category == 'User')
            self.assertNotIn(user.source.path, s.selected_files)
            s.action('next', {'presets': user.id})
            s.action('next', {'backup': '0'})
            s.action('next', {'operation': 'disable'});self.finish_work()
            self.assertEqual([i.source for i in s.plan.items], [user.source.path])
            s.action('next');self.finish_work()
            self.assertEqual(user.source.path.read_bytes(), b'<x cabActive="0"/>')
            self.assertEqual((root / 'Factory/Nested/Tone.xml').read_bytes(), b'<x cabActive="1"/>')
            self.assertEqual((root / 'Artist/Nested/Tone.xml').read_bytes(), b'<x cabActive="1"/>')

    def test_real_pipe_service_review_and_apply(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve();path = root / 'Factory' / '音🎸.xml'
            path.parent.mkdir();path.write_bytes(b'<x cabActive="1"/>')
            process = subprocess.Popen([sys.executable, '-m', 'cabdisabler.backend.server'],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            lines = queue.Queue()
            def read():
                for line in process.stdout:
                    lines.put(line.rstrip('\n'))
            reader = threading.Thread(target=read, daemon=True);reader.start()
            def state():
                head = None
                while True:
                    line = lines.get(timeout=10)
                    self.assertFalse(line.startswith('ERROR'), line)
                    if line.startswith('STATE\t'):
                        head = line.split('\t')
                    if line == 'END':
                        return head
            def send(action, **values):
                process.stdin.write(action + ''.join('\t' + k + '=' + encode(v) for k, v in values.items()) + '\n')
                process.stdin.flush()
                return state()
            try:
                initial = state()
                self.assertEqual(initial[1], '0')
                if initial[2] == '1':
                    state()
                self.assertEqual(send('add', path=str(root))[2], '1')
                self.assertEqual(state()[2], '0')
                self.assertEqual(send('next', plugins='0')[1], '1')
                self.assertEqual(send('next', presets='2')[1], '2')
                self.assertEqual(send('next', backup='0')[1], '3')
                self.assertEqual(send('next', operation='disable')[2], '1')
                self.assertEqual(state()[1], '4')
                self.assertEqual(path.read_bytes(), b'<x cabActive="1"/>')
                self.assertEqual(send('next')[2], '1')
                final = state()
                self.assertEqual(final[1], '5')
                self.assertEqual(final[9:11], ['1', '1'])
                self.assertEqual(path.read_bytes(), b'<x cabActive="0"/>')
            finally:
                process.stdin.write('quit\n');process.stdin.flush();process.stdin.close()
                process.wait(timeout=10);reader.join(timeout=10)
                self.assertEqual(process.stderr.read(), '')
                process.stdout.close();process.stderr.close()
            self.assertEqual(process.returncode, 0)


if __name__ == '__main__':
    unittest.main()
