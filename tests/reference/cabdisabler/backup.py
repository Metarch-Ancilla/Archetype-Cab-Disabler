"""Optional per-run ZIP backups. A complete verified archive precedes any edits."""
from datetime import datetime
from pathlib import Path
import hashlib
import json
import os
import stat
import sys
import zipfile
from .errors import Unsupported
from .files import digest, identity, read_snapshot, safe_path, signature


class BackupCancelled(Unsupported):
    pass


def desktop_directory() -> Path:
    """Use the redirected Windows Desktop, otherwise the user's Desktop folder."""
    if sys.platform == 'win32':
        import ctypes
        from ctypes import wintypes
        # FOLDERID_Desktop, in GUID little-endian byte order. A constant does not
        # require uuid's random/network/platform support in an offline bundle.
        guid = (ctypes.c_byte * 16).from_buffer_copy(bytes.fromhex('3accbfb42cdb4c42b0297fe99a87c641'))
        folder = ctypes.c_wchar_p()
        shell = ctypes.WinDLL('shell32', use_last_error=True)
        get_folder = shell.SHGetKnownFolderPath
        get_folder.argtypes = (ctypes.c_void_p, wintypes.DWORD, wintypes.HANDLE,
                               ctypes.POINTER(ctypes.c_wchar_p))
        get_folder.restype = ctypes.c_long
        if get_folder(ctypes.byref(guid), 0, None, ctypes.byref(folder)) == 0:
            try:
                return Path(folder.value).resolve()
            finally:
                free = ctypes.WinDLL('ole32').CoTaskMemFree
                free.argtypes = (ctypes.c_void_p,)
                free(folder)
    return (Path.home() / 'Desktop').resolve()


def default_backup_path() -> Path:
    folder = desktop_directory()
    stem = 'NeuralDSP presets ' + datetime.now().strftime('%Y-%m-%d')
    candidate = folder / (stem + '.zip')
    number = 2
    while candidate.exists():
        candidate = folder / f'{stem} ({number}).zip'
        number += 1
    return candidate


def create_backup(plan, destination: Path, cancel=None, progress=None) -> Path | None:
    """Archive only files the immutable plan will change; no writes to presets.

    Publication uses exclusive creation, supported on filesystems without hard
    links. An incomplete ZIP is removed on error/cancellation. No pre-existing
    destination is overwritten. Source content and identity must match preview.
    """
    if not plan.complete:
        raise Unsupported('Preview incomplete; review before creating a backup')
    items = [item for item in plan.items if item.status == 'ready']
    if not items:
        return None
    destination = Path(os.path.abspath(destination))
    if destination.suffix.lower() != '.zip':
        raise Unsupported('Choose a .zip backup filename')
    safe_path(destination)
    if destination.exists():
        raise FileExistsError(f'Backup already exists: {destination}; choose another filename')
    # Unique source identities and root groups retain nested folders across drives.
    unique = {}
    for item in items:
        key = identity(item.source)
        if not item.source_signature:
            raise Unsupported('Backup requires a reviewed source signature')
        unique.setdefault(key, item)
    items = list(unique.values())
    groups = {}
    for item in items:
        groups.setdefault(item.source.anchor, []).append(item.source.parent)
    roots = {anchor: Path(os.path.commonpath(paths)) for anchor, paths in groups.items()}
    prefixes = {anchor: f'presets/root-{i:02d}' for i, anchor in enumerate(sorted(roots), 1)}
    destination.parent.mkdir(parents=True, exist_ok=True)
    safe_path(destination)
    owned_identity = None
    try:
        with destination.open('xb') as stream:
            st = os.fstat(stream.fileno())
            owned_identity = (st.st_dev, st.st_ino)
            records = []
            with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
                for number, item in enumerate(items, 1):
                    if cancel and cancel.is_set():
                        raise BackupCancelled('Backup cancelled; no presets edited')
                    if signature(item.source) != item.source_signature:
                        raise Unsupported(f'Source changed since preview: {item.source}')
                    data, snap = read_snapshot(item.source)
                    if digest(data) != item.before_hash or snap != item.source_signature:
                        raise Unsupported(f'Source changed since preview: {item.source}')
                    relative = item.source.relative_to(roots[item.source.anchor]).as_posix()
                    name = prefixes[item.source.anchor] + '/' + relative
                    info = zipfile.ZipInfo(name)
                    info.compress_type = zipfile.ZIP_DEFLATED
                    try:
                        modified = datetime.fromtimestamp(item.source_signature[3] / 1_000_000_000)
                        if 1980 <= modified.year <= 2107:
                            info.date_time = modified.timetuple()[:6]
                    except (OverflowError, OSError, ValueError):
                        pass  # The manifest retains the exact original timestamp.
                    info.external_attr = item.source_signature[5] << 16
                    archive.writestr(info, data, compresslevel=1)
                    records.append({'entry': name, 'source': str(item.source), 'sha256': item.before_hash,
                                    'mtime_ns': item.source_signature[3],
                                    'mode': stat.S_IMODE(item.source_signature[5])})
                    if progress:
                        progress(number, len(items))
                manifest = {'format': 1, 'description': 'Original preset bytes before this batch', 'files': records}
                manifest_bytes = json.dumps(manifest, ensure_ascii=False, indent=2).encode()
                archive.writestr('backup-manifest.json', manifest_bytes)
            stream.flush()
            os.fsync(stream.fileno())
        archive_signature = signature(destination)
        if archive_signature[:2] != owned_identity:
            raise Unsupported('Backup destination replaced during creation')
        with zipfile.ZipFile(destination) as archive:
            if archive.read('backup-manifest.json') != manifest_bytes:
                raise Unsupported('Backup manifest verification failed')
            for record in records:
                if cancel and cancel.is_set():
                    raise BackupCancelled('Backup verification cancelled; no presets edited')
                with archive.open(record['entry']) as entry:
                    checksum = hashlib.sha256()
                    while block := entry.read(65536):
                        checksum.update(block)
                if checksum.hexdigest() != record['sha256']:
                    raise Unsupported('Backup verification failed; no presets edited')
        if signature(destination) != archive_signature:
            raise Unsupported('Backup destination changed during creation')
        return destination
    except BaseException as exc:
        cleanup_error = None
        if owned_identity is not None:
            try:
                if signature(destination)[:2] == owned_identity:
                    destination.unlink()
            except OSError as cleanup:
                cleanup_error = cleanup
        if cleanup_error is not None and isinstance(exc, Exception):
            raise Unsupported(f'{exc}; incomplete ZIP could not be removed: {destination}: {cleanup_error}') from exc
        if isinstance(exc, (zipfile.BadZipFile, zipfile.LargeZipFile)):
            raise Unsupported('Backup ZIP validation failed') from exc
        raise
