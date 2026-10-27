"""Private library audit; outputs counts/schema evidence, never preset contents.

Uses the test-only independent tree reader to verify the native backend's byte
spans. All edits operate on temporary copies. Source presets remain untouched.
"""
import argparse
import collections
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/reference'))
from cabdisabler import valuetree, xmltree


def inspect(backend, paths):
    result = []
    for start in range(0, len(paths), 100):
        run = subprocess.run([backend, 'inspect', *map(str, paths[start:start + 100])],
                             capture_output=True, text=True, check=True)
        result.extend(json.loads(line) for line in run.stdout.splitlines())
    assert len(result) == len(paths), 'Incomplete inspection'
    return result


def scalars(data):
    try:
        root = xmltree.parse(data).root
    except ValueError:
        root = valuetree.parse(data)
    result = []
    def walk(node, path):
        path += '/' + node.name
        if node.name == 'PARAM':
            path += '[' + str(node.property('id').value) + ']'
        for scalar in node.properties:
            result.append((path + '@' + scalar.name, scalar))
        if node.text is not None:
            result.append((path + '#text', node.text))
        for child in node.children:
            walk(child, path)
    walk(root, '')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', type=Path, required=True)
    parser.add_argument('--presets', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    backend = str(args.backend.resolve())
    paths = sorted(p for p in args.presets.rglob('*') if p.is_file() and
                   p.suffix.lower() == '.xml' and '__MACOSX' not in p.parts and
                   not p.name.startswith('._'))
    assert paths, 'No XML files'
    originals = [p.read_bytes() for p in paths]
    infos = inspect(backend, paths)
    groups = collections.defaultdict(collections.Counter)
    formats = collections.Counter()
    switches = collections.Counter()
    valid = []
    for path, data, info in zip(paths, originals, infos):
        group = path.relative_to(args.presets).parts[0]
        groups[group]['total'] += 1
        if not info.get('adapter'):
            groups[group]['unsupported'] += 1
            continue
        groups[group]['supported'] += 1
        adapter = info['adapter']
        formats[adapter['format']] += 1
        matches = [(name, value) for name, value in scalars(data)
                   if [value.start, value.end] == adapter['span']]
        assert len(matches) == 1, 'Native span is not one independently parsed scalar'
        name, scalar = matches[0]
        switches[name.split('/')[-1] + ':' + scalar.kind] += 1
        valid.append((data, adapter, name))
    with tempfile.TemporaryDirectory(prefix='cab-library-audit-') as directory:
        copies = [Path(directory) / f'{i}.xml' for i in range(len(valid))]
        for desired in (False, True):
            for path, (data, _, _) in zip(copies, valid):
                path.write_bytes(data)
            for start in range(0, len(copies), 100):
                subprocess.run([backend, 'edit', 'enable' if desired else 'disable',
                                *map(str, copies[start:start + 100]), '--apply'],
                               stdout=subprocess.DEVNULL, check=True)
            edited = inspect(backend, copies)
            for path, (data, before, name), result in zip(copies, valid, edited):
                output = path.read_bytes()
                after = result['adapter']
                assert after['cab_enabled'] == [desired], 'Wrong native state'
                a, b = before['span']; c, d = after['span']
                assert data[:a] == output[:c] and data[b:] == output[d:], 'Unrelated bytes changed'
                before_tree, after_tree = scalars(data), scalars(output)
                assert len(before_tree) == len(after_tree)
                for (old_name, old), (new_name, new) in zip(before_tree, after_tree):
                    assert old_name == new_name and old.kind == new.kind
                    if old_name != name:
                        assert old.value == new.value, 'Unrelated scalar changed'
                    else:
                        value = str(new.value).lower()
                        active = value == 'true' if value in ('true', 'false') else float(value) == 1
                        assert active == desired, 'Independent scalar state mismatch'
                if before['cab_enabled'] == [desired]:
                    assert output == data, 'No-op changed bytes'
    assert all(p.read_bytes() == data for p, data in zip(paths, originals)), 'Source changed'
    report = dict(total=len(paths), supported=len(valid), unsupported=len(paths)-len(valid),
                  products={k:dict(v) for k,v in sorted(groups.items())},
                  formats=dict(formats), switches=dict(switches),
                  both_directions_validated=True, single_scalar_only=True,
                  source_files_unchanged=True,
                  scope='Structural validation only; no paired/plugin audio verification')
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
