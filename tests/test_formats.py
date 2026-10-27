from pathlib import Path
import tempfile
import unittest
from cabdisabler.adapters import PRODUCTION, Unsupported
from cabdisabler.engine import Source, preview, apply
from tests.test_valuetree import node


class FormatTests(unittest.TestCase):
    def roundtrip(self, data):
        adapter, info = PRODUCTION.identify(data)
        self.assertEqual(len(info.cab_spans), 1)
        output = adapter.patch(data, False)
        changed = adapter.inspect(output)
        a,b = info.cab_spans[0];c,d = changed.cab_spans[0]
        self.assertEqual(data[:a], output[:c])
        self.assertEqual(data[b:], output[d:])
        self.assertEqual(adapter.patch(output, True), data)
        self.assertEqual(adapter.patch(output, False), output)
        return adapter, info

    def test_xml_param_encoding_bom_formatting_and_unicode(self):
        text = ('<?xml version="1.0"?>\r\n<!-- retain comments -->\r\n'
                '<ArchetypeFuture name="音🎸 &amp; tone" version="200.9">\r\n'
                "  <PARAM id='cabSectionActive' value = '1.000' />\r\n"
                '  <PARAM id="cab1Active" value="0"/>\r\n'
                '  <PARAM id="irPath" value="/日本語/音.wav"/>\r\n'
                '</ArchetypeFuture>\r\n')
        for encoding,bom in (('utf-8',b''), ('utf-8',b'\xef\xbb\xbf'),
                             ('utf-16-le',b'\xff\xfe'), ('utf-16-be',b'\xfe\xff')):
            with self.subTest(encoding=encoding,bom=bom):
                adapter,info=self.roundtrip(bom+text.encode(encoding))
                self.assertEqual(adapter.id,'text.xml')
                self.assertEqual(info.display_name,'音🎸 & tone')

    def test_modern_xml_context_excludes_amp_and_mic_switches(self):
        data = (b'<any-x version="999" name="Tone"><appModel><subModels><parameters><subModels>'
                b'<ampParameters sectionActive="false"/>'
                b'<cabParameters sectionActive="true"><mic sectionActive="false"/></cabParameters>'
                b'</subModels></parameters></subModels></appModel></any-x>')
        self.roundtrip(data)

    def test_global_cab_property_and_string_styles(self):
        for value in ('true','True','TRUE','1','1.0','1.0000'):
            with self.subTest(value=value):
                self.roundtrip(f'<Future cabActive="{value}" name="Tone"/>'.encode())

    def test_unseen_binary_roots_versions_and_types(self):
        for value in (True,1,1.0,'true','1.000'):
            for root in ('archetype-not-in-tests-before-x','neural_dsp_unreleased','new-format-root'):
                with self.subTest(value=value,root=root):
                    data=node(root,[('version','123456.9'),('name','Tone')],
                        [node('PARAM',[('id','cabActive'),('value',value)]),
                         node('PARAM',[('id','other'),('value','untouched')])])
                    self.roundtrip(data)

    def test_extra_settings_do_not_require_new_adapter(self):
        data=node('unseen-archetype-x',[('name','Tone'),('customBinary',b'\0\xff'),('version','Next')],
                  [node('cabParameters',[('sectionActive','true'),('extra','unchanged')]),
                   node('newEffect',[('parameter',0.237)])])
        self.roundtrip(data)

    def test_element_property_xml_and_unrelated_text_are_preserved(self):
        text = ('<any-archetype><name>音🎸 &amp; notes</name><version>future</version>'
                '<notes>Unrelated text, punctuation and Unicode 🎸 stay exactly as saved.</notes>'
                '<PARAM>\n <id>cabSectionActive</id>\n <value>1.0</value>\n</PARAM>'
                '<PARAM><id>cab1Active</id><value>0</value></PARAM></any-archetype>')
        for encoding in ('utf-8', 'utf-16'):
            with self.subTest(encoding=encoding):
                self.roundtrip(text.encode(encoding))

    def test_target_xml_mixed_text_and_entities_are_rejected(self):
        for data in (b'<x><PARAM id="cabActive" value="1">ambiguous</PARAM></x>',
                     b'<x><PARAM><id>cabActive</id><value>tr&#117;e</value></PARAM></x>',
                     b'<x><PARAM><id>cabActive</id><value>1</value><value>0</value></PARAM></x>'):
            with self.assertRaises(Unsupported):
                PRODUCTION.identify(data)

    def test_hostile_xml_and_ambiguous_target_are_untouched(self):
        bad=[b'<!DOCTYPE x [<!ENTITY a SYSTEM "file:///secret">]><x cabActive="true"/>',
             b'<x cabActive="true" cabActive="false"/>',
             b'<x><PARAM id="cabActive" value="1"/><PARAM id="cabSectionActive" value="1"/></x>',
             b'<x><PARAM id="cabActive" value="1"/><PARAM id="cabActive" value="1"/></x>',
             b'<x><mic sectionActive="true"/><PARAM id="cab1Active" value="1"/></x>',
             b'<x><cabParameters sectionActive="true"/><cabParameters sectionActive="false"/></x>',
             b'<x cabActive="tr&#117;e"/>', b'<x cabActive="0.5"/>',
             b'<x cabActive="true"><unfinished>',
             b'<?xml version="1.0" encoding="ISO-8859-1"?><x cabActive="true"/>',
             b'<x>'*66+b'</x>'*66]
        for data in bad:
            with self.subTest(data=data[:70]),self.assertRaises(Unsupported):
                PRODUCTION.identify(data)

    def test_xml_engine_changes_only_flag_and_reenables_after_restart(self):
        data=b'<unknown-new-plugin version="200"><PARAM id="cabActive" value="1"/><PARAM id="mic" value="0"/></unknown-new-plugin>'
        with tempfile.TemporaryDirectory() as d:
            path=Path(d).resolve()/'Tone.xml';path.write_bytes(data)
            def plan(op):return preview((Source(path,'Any product',path.parent),),op)
            self.assertEqual(apply(plan('disable'))[0].status,'changed')
            self.assertEqual(apply(plan('disable'))[0].status,'already-correct')
            import subprocess,sys
            code=('from pathlib import Path;from cabdisabler.engine import Source,preview,apply;'
                  f'p=Path({str(path)!r});'
                  'assert apply(preview((Source(p,"No plugin whitelist",p.parent),),"enable"))[0].status=="changed"')
            subprocess.run([sys.executable,'-c',code],check=True)
            self.assertEqual(path.read_bytes(),data)

    def test_unknown_encrypted_or_compressed_data_is_rejected(self):
        for data in (b'\x1f\x8bcompressed',b'PK\x03\x04cabActive',b'\0cabActive\x01',b'not-a-preset'):
            with self.assertRaises(Unsupported):PRODUCTION.identify(data)

    def test_legacy_param_id_is_independent_of_mic_states_and_filename(self):
        for first, second in ((1.0, 1.0), (1.0, 0.0), (0.0, 0.0)):
            data = node('unseen-legacy', [('name', 'Tone no cab')], [
                node('PARAM', [('id', 'cabActiveID'), ('value', 1.0)]),
                node('PARAM', [('id', 'cab1ActiveID'), ('value', first)]),
                node('PARAM', [('id', 'cab2ActiveID'), ('value', second)])])
            with self.subTest(first=first, second=second):
                adapter, info = self.roundtrip(data)
                self.assertEqual(info.enabled, (True,))
                self.assertIn('[cabActiveID]', info.schema)
                self.assertFalse(adapter.inspect(adapter.patch(data, False)).enabled[0])

    def test_legacy_param_id_types_and_xml_representations(self):
        for value in (True, 1, 1.0, 'true', '1.000'):
            with self.subTest(value=value):
                self.roundtrip(node('new-identity', [('version', 'future')], [
                    node('PARAM', [('id', 'cabActiveID'), ('value', value)])]))
        for text in (
            '<unknown><PARAM id="cabActiveID" value="1.000"/></unknown>',
            '<unknown><PARAM><id>cabActiveID</id><value>true</value></PARAM></unknown>',
        ):
            for encoding in ('utf-8', 'utf-16'):
                with self.subTest(text=text, encoding=encoding):
                    self.roundtrip(text.encode(encoding))

    def test_legacy_param_id_does_not_relax_ambiguity_or_context(self):
        for data in (
            b'<x><PARAM id="cabActiveID" value="1"/><PARAM id="cabActive" value="1"/></x>',
            b'<x><PARAM id="cabActiveID" value="1"/><PARAM id="cabActiveID" value="0"/></x>',
            b'<x><PARAM id="cabActiveID" value="0.5"/></x>',
            b'<x><PARAM id="cabActiveID" value="1" extra="ambiguous"/></x>',
            b'<x cabActiveID="1"/>',
            b'<x><PARAM id="cab1ActiveID" value="1"/><PARAM id="cab2ActiveID" value="1"/></x>',
            b'<x><PARAM id="othercabActiveID" value="1"/></x>',
        ):
            with self.subTest(data=data), self.assertRaises(Unsupported):
                PRODUCTION.identify(data)
