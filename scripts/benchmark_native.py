"""Measure native CLI preview throughput with temporary, generated XML presets."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', type=Path, required=True)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--count', type=int, default=1000)
    parser.add_argument('--samples', type=int, default=3)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.count <= 2000 or not 1 <= args.samples <= 10: parser.error('Use 1–2000 presets and 1–10 samples')
    binaries = {'current': args.backend.resolve()}
    if args.baseline: binaries['baseline'] = args.baseline.resolve()
    result = {name: dict(version=subprocess.check_output([str(binary), '--version'], text=True).strip(), preview_seconds=[]) for name, binary in binaries.items()}
    payload = b'<future name="Generated benchmark" version="1.0" cabActive="true">' + b''.join(f'<PARAM id="control-{i}" value="0.5"/>'.encode() for i in range(32)) + b'</future>'
    with tempfile.TemporaryDirectory(prefix='cab-native-benchmark-') as directory:
        files = [Path(directory) / f'Tone{i:04}.xml' for i in range(args.count)]
        for path in files: path.write_bytes(payload)
        batches = [[]]
        executable = max(map(str, binaries.values()), key=len)
        for path in files:
            batch = batches[-1]
            if os.name == 'nt' and batch and len(subprocess.list2cmdline([executable, 'edit', 'disable', *map(str, batch), str(path)])) > 24000:
                batches.append([])
            batches[-1].append(path)
        for sample in range(args.samples):
            order = list(binaries.items())
            if sample % 2: order.reverse()
            for name, binary in order:
                start = time.perf_counter()
                for batch in batches:
                    subprocess.run([str(binary), 'edit', 'disable', *map(str, batch)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=True, timeout=120)
                result[name]['preview_seconds'].append(time.perf_counter() - start)
        expected = hashlib.sha256(payload).digest()
        assert all(hashlib.sha256(path.read_bytes()).digest() == expected for path in files), 'Preview modified a preset'
    for item in result.values():
        item['median_seconds'] = statistics.median(item['preview_seconds'])
        item['presets_per_second'] = args.count / item['median_seconds']
    report = dict(presets=args.count, bytes_per_preset=len(payload), results=result,
                  scope='Generated XML CLI preview; local host and warm filesystem; no native GUI, package-size or cold-start claim')
    args.report.write_text(json.dumps(report, indent=2) + '\n'); print(json.dumps(report, indent=2))


if __name__ == '__main__': main()
