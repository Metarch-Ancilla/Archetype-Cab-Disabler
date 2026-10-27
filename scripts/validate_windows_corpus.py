"""Check Windows backend output against a private host-generated oracle corpus.

Corpus data stays outside source/packages. Only aggregate validation is reported.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', type=Path, required=True)
    parser.add_argument('--corpus', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args(); binary = str(args.backend.resolve())
    corpus = json.loads(args.corpus.read_text()); records = corpus['cases']
    with tempfile.TemporaryDirectory(prefix='cab-compiler-corpus-') as directory:
        root = Path(directory); paths = [root / f'{i}.xml' for i in range(len(records))]
        for path, record in zip(paths, records): path.write_bytes(base64.b64decode(record['input']))
        native = []
        for start in range(0, len(paths), 100):
            run = subprocess.run([binary, 'inspect', *map(str, paths[start:start+100])], capture_output=True, text=True, check=True, timeout=30)
            native.extend(json.loads(line) for line in run.stdout.splitlines())
        assert len(native) == len(records), 'Missing inspection results'
        for i, (info, record) in enumerate(zip(native, records)):
            assert bool(info.get('adapter')) == record['accepted'], f'Acceptance mismatch for case {i}'
        valid = [(path, record) for path, record in zip(paths, records) if record['accepted']]
        for desired in ('disabled', 'enabled'):
            for path, record in valid: path.write_bytes(base64.b64decode(record['input']))
            for start in range(0, len(valid), 100):
                subprocess.run([binary, 'edit', 'enable' if desired == 'enabled' else 'disable', *[str(p) for p, _ in valid[start:start+100]], '--apply'], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=True, timeout=30)
            for i, (path, record) in enumerate(valid):
                assert hashlib.sha256(path.read_bytes()).hexdigest() == record[desired], f'Output mismatch for valid case {i}'
    accepted = sum(record['accepted'] for record in records)
    report = dict(inputs=len(records), accepted=accepted, rejected=len(records)-accepted,
                  private_exports=corpus['private_exports'], reference_tests_on_host=corpus['reference_tests'],
                  enabled_disabled_hashes_match=True, scope='Actual Windows C++ executable vs host oracle; no private bytes in report')
    args.report.write_text(json.dumps(report, indent=2)+'\n');print(json.dumps(report))


if __name__ == '__main__': main()
