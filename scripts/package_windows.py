"""Create a runtime-only Windows download; keep build evidence beside the ZIP."""
from pathlib import Path
import zipfile


def package_windows(root, archive):
    root, archive = Path(root), Path(archive)
    # Build tools may retain intermediates; the public archive contains one EXE.
    files = [(root / 'Archetype Cab Disabler.exe', Path('Archetype Cab Disabler.exe'))]
    for source, _ in files:
        if not source.is_file() or source.is_symlink():
            raise ValueError(f'Missing or linked package file: {source}')
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED, compresslevel=9) as stream:
        for source, relative in files:
            stream.write(source, relative)
    with zipfile.ZipFile(archive) as stream:
        if stream.testzip():
            raise ValueError('Windows ZIP integrity check failed')
    return archive
