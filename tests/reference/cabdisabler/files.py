"""Shared bounded file reads and verified writes to existing preset files."""
from pathlib import Path
import hashlib
import os
import stat
from .errors import MAX_BYTES, Unsupported


def absolute(path: Path) -> Path:
    return Path(os.path.abspath(path))


def safe_path(path: Path) -> None:
    path = absolute(path)
    for part in (path, *path.parents):
        if part.is_symlink() or getattr(part, 'is_junction', lambda: False)():
            raise Unsupported('Symbolic-link/junction paths are not writable')


def _signature(st) -> tuple[int, ...]:
    return (st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns,
            st.st_mode, st.st_uid, st.st_gid)


def signature(path: Path) -> tuple[int, ...]:
    if os.name == 'nt':
        # Windows path stat and handle stat can expose different ctime values
        # after ReplaceFileW. Compare handle information with handle information,
        # including when read_snapshot checks that its open file is still current.
        with path.open('rb') as stream:
            return _signature(os.fstat(stream.fileno()))
    return _signature(path.stat())


def identity(path: Path, st=None):
    st = st or path.stat()
    # Some network/removable filesystems report inode zero for every file.
    # Do not silently deduplicate unrelated files in that case.
    return (st.st_dev, st.st_ino) if st.st_ino else os.path.normcase(str(absolute(path)))


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_snapshot(path: Path) -> tuple[bytes, tuple[int, ...]]:
    path = absolute(path)
    safe_path(path)
    if not stat.S_ISREG(path.lstat().st_mode):
        raise Unsupported('Not a regular file')
    flags = (os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0)
             | getattr(os, 'O_NONBLOCK', 0) | getattr(os, 'O_BINARY', 0))
    fd = os.open(path, flags)
    with os.fdopen(fd, 'rb') as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise Unsupported('Not a regular file')
        if before.st_size > MAX_BYTES:
            raise Unsupported('Preset exceeds 4 MiB safety limit')
        data = stream.read(MAX_BYTES + 1)
        snap = _signature(before)
        if _signature(os.fstat(stream.fileno())) != snap or signature(path) != snap:
            raise Unsupported('Source changed while reading; review again')
    if len(data) > MAX_BYTES:
        raise Unsupported('Preset exceeds 4 MiB safety limit')
    return data, snap


def read_preset(path: Path) -> bytes:
    return read_snapshot(path)[0]



def write_original(target: Path, output: bytes, expected_signature, expected_hash: str, opener=None) -> None:
    """Overwrite the opened original without truncating first or replacing its inode.

    Ownership, permissions, ACLs and hard-link identity stay on the existing file.
    A write interruption is not atomic; an optional verified ZIP precedes apply.
    """
    target = absolute(target)
    safe_path(target)
    flags = (os.O_RDWR | getattr(os, 'O_NOFOLLOW', 0)
             | getattr(os, 'O_NONBLOCK', 0) | getattr(os, 'O_BINARY', 0))
    fd = (opener or os.open)(target, flags)  # Existing file only; never create or truncate here.
    started = False
    try:
        with os.fdopen(fd, 'r+b') as stream:
            st = os.fstat(stream.fileno())
            if not stat.S_ISREG(st.st_mode) or _signature(st) != expected_signature:
                raise Unsupported('Source changed before direct write; review again')
            before = stream.read(MAX_BYTES + 1)
            if digest(before) != expected_hash or _signature(os.fstat(stream.fileno())) != expected_signature:
                raise Unsupported('Source changed before direct write; review again')
            safe_path(target)
            if signature(target) != expected_signature:
                raise Unsupported('Source changed before direct write; review again')
            stream.seek(0)
            started = True
            remaining = memoryview(output)
            while remaining:
                count = stream.write(remaining)
                if not count:
                    raise OSError('Direct write made no progress')
                remaining = remaining[count:]
            # Cab encodings may change length. Remove a stale tail only after writing.
            stream.truncate(len(output))
            stream.flush()
            os.fsync(stream.fileno())
            stream.seek(0)
            if stream.read(MAX_BYTES + 1) != output:
                raise Unsupported('Direct write read-back verification failed')
            safe_path(target)
            if signature(target) != _signature(os.fstat(stream.fileno())):
                raise Unsupported('Source changed during direct write')
    except (OSError, ValueError) as exc:
        if started:
            raise Unsupported(f'Direct write failed; file may be partially modified. Restore from your backup if needed: {exc}') from exc
        raise
