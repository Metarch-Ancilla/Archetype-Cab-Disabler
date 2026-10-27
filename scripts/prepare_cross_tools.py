"""Prepare cached Linux-to-Windows/macOS packaging tools in the workspace, without root."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
INPUTS = [
    ('https://github.com/joseluisq/macosx-sdks/releases/download/13.3/MacOSX13.3.sdk.tar.xz',
     '518e35eae6039b3f64e8025f4525c1c43786cc5cf39459d609852faf091e34be'),
    ('https://github.com/indygreg/apple-platform-rs/releases/download/apple-codesign/0.29.0/apple-codesign-0.29.0-x86_64-unknown-linux-musl.tar.gz',
     'dbe85cedd8ee4217b64e9a0e4c2aef92ab8bcaaa41f20bde99781ff02e600002'),
]
PACKAGES = ['clang-19', 'libclang-cpp19', 'libllvm19', 'llvm-19', 'lld-19', 'libclang-common-19-dev',
            'g++-mingw-w64-x86-64-posix', 'gcc-mingw-w64-x86-64-posix',
            'gcc-mingw-w64-x86-64-posix-runtime', 'mingw-w64-x86-64-dev',
            'mingw-w64-common', 'binutils-mingw-w64-x86-64',
            'wine64', 'libwine', 'libcapi20-3t64', 'libpcap0.8t64', 'libxkbregistry0', 'libz-mingw-w64', 'xvfb']


def download(url, directory, expected, filename=None):
    path = directory / (filename or url.rsplit('/', 1)[-1])
    if not path.exists():
        print('Downloading', path.name, flush=True)
        partial = path.with_name(path.name + '.partial')
        with urllib.request.urlopen(url, timeout=60) as source, partial.open('wb') as target:
            shutil.copyfileobj(source, target)
        if hashlib.sha256(partial.read_bytes()).hexdigest() != expected:
            raise ValueError(f'Checksum mismatch: {path.name}')
        partial.rename(path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise ValueError(f'Cached input checksum mismatch: {path.name}')
    return path


def debian_tools(tools):
    apt = tools.parent / 'apt'
    for relative in ['lists/partial', 'cache/archives/partial']:
        (apt / relative).mkdir(parents=True, exist_ok=True)
    (apt / 'sources.list').write_text('deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian trixie main\n')
    config = apt / 'config'
    config.write_text(f'Dir::Etc::main "/dev/null";\nDir::Etc::parts "-";\nDir::Etc::sourcelist "{apt}/sources.list";\nDir::Etc::sourceparts "-";\nDir::State::lists "{apt}/lists";\nDir::Cache "{apt}/cache";\nDebug::NoLocking "true";\n')
    env = dict(os.environ, APT_CONFIG=str(config))
    index = apt / 'lists/deb.debian.org_debian_dists_trixie_main_binary-amd64_Packages'
    if not index.exists():
        subprocess.run(['apt-get', '-o', 'Acquire::GzipIndexes=false', 'update'], env=env, check=True)
    entries = {}
    for paragraph in index.read_text().split('\n\n'):
        fields = dict(line.split(': ', 1) for line in paragraph.splitlines()
                      if ': ' in line and not line.startswith(' '))
        if fields.get('Package') in PACKAGES:
            entries[fields['Package'], fields['Version'], fields['Architecture']] = fields
    manifest = tools / 'debian-inputs.json'
    records = json.loads(manifest.read_text()) if manifest.exists() else []
    known = {record['package'] for record in records}
    missing = [name for name in PACKAGES if name not in known]
    if missing:
        subprocess.run(['apt-get', 'download', *missing], cwd=tools / 'debs', env=env, check=True)
    by_file = {record['file']: record for record in records}
    for path in sorted((tools / 'debs').glob('*.deb')):
        key = tuple(subprocess.check_output(['dpkg-deb', '-f', str(path), field], text=True).strip()
                    for field in ['Package', 'Version', 'Architecture'])
        if key[0] not in PACKAGES:
            continue
        expected = entries[key]
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != expected['SHA256']:
            raise ValueError(f'Debian index hash mismatch: {path.name}')
        if path.name not in by_file:
            subprocess.run(['dpkg-deb', '-x', str(path), str(tools / 'root')], check=True)
            by_file[path.name] = {'file': path.name, 'sha256': digest, 'version': key[1], 'package': key[0]}
    manifest.write_text(json.dumps(list(by_file.values()), indent=2) + '\n')
    # Relocate Debian Wine helpers/data without changing the system installation.
    server = tools / 'root/usr/lib/wine/wineserver'
    server.write_text('#!/bin/sh\nexec "$(dirname "$0")/wineserver64" -p0 "$@"\n')
    server.chmod(0o755)
    nls = tools / 'root/usr/share/wine/wine/nls'
    nls.parent.mkdir(exist_ok=True)
    if not nls.exists():
        nls.symlink_to('../nls')
    zlib_dll = tools / 'root/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/zlib1.dll'
    if not zlib_dll.exists():
        zlib_dll.symlink_to('../../../../x86_64-w64-mingw32/lib/zlib1.dll')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, default=Path('/workspace/tools/cab-cross'))
    args = parser.parse_args()
    tools = args.tools.resolve()
    for directory in ['downloads', 'debs', 'root', 'cache', 'notices']:
        (tools / directory).mkdir(parents=True, exist_ok=True)
    debian_tools(tools)
    records = []
    for url, digest in INPUTS:
        path = download(url, tools / 'downloads', digest)
        records.append({'url': url, 'sha256': digest, 'file': path.name, 'bytes': path.stat().st_size})
    (tools / 'download-inputs.json').write_text(json.dumps(records, indent=2) + '\n')
    for filename, destination, marker in [
        ('MacOSX13.3.sdk.tar.xz', 'sdk', 'MacOSX13.3.sdk/SDKSettings.json'),
        ('apple-codesign-0.29.0-x86_64-unknown-linux-musl.tar.gz', 'codesign', 'apple-codesign-0.29.0-x86_64-unknown-linux-musl/rcodesign')]:
        if not (tools / destination / marker).exists():
            with tarfile.open(tools / 'downloads' / filename) as archive:
                archive.extractall(tools / destination, filter='data')
    print('Prepared cached tools. See docs/CROSS-BUILDING.md for Wine initialization and build commands.')


if __name__ == '__main__':
    main()
