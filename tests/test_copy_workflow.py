"""Production format copy mode: selected files only, original bytes retained."""
from pathlib import Path
import tempfile
import unittest
import zipfile

from cabdisabler.adapters import PRODUCTION
from cabdisabler.engine import Source, preview, apply
from cabdisabler.backend.session import Session
from cabdisabler.discovery import Root, discover


class CopyWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.library = Path(self.temp.name).resolve() / 'Archetype Future' / 'Presets'
        self.original = b'<unseen version="99" name="Original" uid="fixed"><PARAM id="cabActiveID" value="1.000"/><PARAM id="cab1ActiveID" value="0"/></unseen>'

    def source(self, relative, data=None):
        path = self.library / relative; path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(self.original if data is None else data)
        return Source(path, 'Archetype Future', self.library)

    def test_default_copy_layout_and_backup_of_originals(self):
        sources = tuple(self.source(p) for p in ('Artist/日本語/Tone.xml', 'Neural DSP/Tone.xml', 'User/Tone.xml'))
        omitted = self.source('Artist/Not selected.xml')
        plan = preview(sources, 'copy')
        backup = self.library.parent / 'backup.zip'
        self.assertTrue(all(i.status == 'ready' for i in plan.items))
        self.assertFalse((self.library / 'Cab Disabled').exists())
        self.assertTrue(all(r.status == 'changed' for r in apply(plan, backup_path=backup)))
        with zipfile.ZipFile(backup) as archive:
            self.assertEqual(sum(archive.read(n) == self.original for n in archive.namelist()), 3)
        for source, item in zip(sources, plan.items):
            relative = source.path.relative_to(self.library)
            self.assertEqual(item.destination, self.library / 'Cab Disabled' / relative.parent / 'Tone.xml')
            self.assertEqual(source.path.read_bytes(), self.original)
            adapter, info = PRODUCTION.identify(self.original)
            self.assertEqual(item.destination.read_bytes(), adapter.patch(self.original, False))
            self.assertEqual(adapter.inspect(item.destination.read_bytes()).enabled, (False,))
            self.assertEqual(len(info.cab_spans), 1)
        self.assertFalse((self.library / 'Cab Disabled/Artist/Not selected.xml').exists())
        self.assertEqual(omitted.path.read_bytes(), self.original)

    def test_already_disabled_source_still_gets_a_copy(self):
        source = self.source('User/Already.xml', self.original.replace(b'1.000', b'0.000'))
        plan = preview((source,), 'copy')
        self.assertEqual(apply(plan)[0].status, 'changed')
        self.assertEqual(plan.items[0].destination.read_bytes(), source.path.read_bytes())
        self.assertEqual(apply(preview((source,), 'copy'))[0].status, 'already-correct')

    def test_copy_collision_never_overwrites(self):
        source = self.source('Artist/Tone.xml'); plan = preview((source,), 'copy')
        target = plan.items[0].destination; target.parent.mkdir(parents=True); target.write_bytes(b'existing')
        self.assertEqual(apply(plan)[0].status, 'failed')
        self.assertEqual(target.read_bytes(), b'existing')
        self.assertEqual(source.path.read_bytes(), self.original)

    def test_generated_copy_is_not_recopied(self):
        source = self.source('Cab Disabled/Artist/Tone.xml')
        self.assertEqual(preview((source,), 'copy').items[0].status, 'skipped')

    def test_direct_edits_never_rename_files(self):
        source = self.source('User/Original.xml'); original_names = sorted(self.library.rglob('*.xml'))
        self.assertEqual(apply(preview((source,), 'disable'))[0].status, 'changed')
        self.assertEqual(sorted(self.library.rglob('*.xml')), original_names)
        self.assertFalse((self.library / 'Cab Disabled').exists())

    def test_backend_copies_only_checked_user_leaf(self):
        first = self.source('User/Included.xml'); excluded = self.source('User/Excluded.xml')
        events = []; session = Session(lambda *event: events.append(event))
        def finish():
            _, kind, operation = next(e for e in events if e[0] == 'work'); events.clear()
            session.finished(kind, operation())
        session.roots = [Root(self.library, product='folder')]
        session.action('scan'); finish(); session.action('next', {'plugins': '0'})
        leaf = next(n for n in session.nodes if n.source and n.source.path == first.path)
        session.action('next', {'presets': leaf.id})
        session.action('next', {'backup': '0'})
        session.action('next', {'operation': 'copy'}); finish()
        self.assertEqual([i.source for i in session.plan.items], [first.path])
        session.action('next'); finish()
        self.assertTrue((self.library / 'Cab Disabled/User/Included.xml').exists())
        self.assertFalse((self.library / 'Cab Disabled/User/Excluded.xml').exists())
        self.assertEqual(first.path.read_bytes(), self.original)
        self.assertEqual(excluded.path.read_bytes(), self.original)

    def test_tree_parent_structure_and_hidden_discovery_roots(self):
        self.source('Artist/Deep/Tone.xml'); self.source('User/Other.xml')
        session = Session(); session.discovery = discover((Root(self.library, product='folder'),))
        view = session.view()
        self.assertEqual([c['kind'] for c in view['controls']], ['plugins', 'buttons'])
        session.action('next', {'plugins': '0'})
        for index, node in enumerate(session.nodes):
            self.assertLess(node.parent, index)
            if node.parent >= 0:
                self.assertIsNone(session.nodes[node.parent].source)
        self.assertEqual(sum(n.source is not None for n in session.nodes), 2)
        self.assertEqual(len({n.source.path for n in session.nodes if n.source}), 2)

    def test_report_export_cannot_replace_a_preset(self):
        source = self.source('User/Tone.xml')
        session = Session();session.page = 5;session.report = 'Results'
        with self.assertRaises(ValueError):
            session.action('export', {'path': str(source.path)})
        self.assertEqual(source.path.read_bytes(), self.original)
