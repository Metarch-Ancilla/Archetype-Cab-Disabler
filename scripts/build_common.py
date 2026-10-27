"""Shared host build metadata and the native application's canonical version."""
import hashlib
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXPAT_SOURCES = ("xmlparse", "xmlrole", "xmltok", "xcs")
VERSION = re.search(r'^#define CAB_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"$',
                    (ROOT / 'shared/version.h').read_text(), re.MULTILINE).group(1)


def source_state():
    return dict(source_commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                source_dirty=bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT)))


def artifact_record(archive):
    with archive.open('rb') as stream: digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return dict(artifact=archive.name, bytes=archive.stat().st_size, sha256=digest)


def license_file(platform, version=VERSION):
    path = ROOT / 'licenses' / (platform + '-LICENSES.txt')
    if f'Version: {version}' not in path.read_text(encoding='utf-8').splitlines()[:3]:
        raise ValueError('License header version mismatch; run scripts/generate_licenses.py before committing build inputs')
    return path
