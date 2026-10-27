"""Bounded preset-folder discovery. Never scan or load plugin binaries."""
from dataclasses import dataclass
from pathlib import Path
import os
import sys
from .engine import Source
from .files import identity, safe_path, absolute

CATEGORY_FOLDERS = {'artist', 'artists', 'factory', 'factory presets', 'neural dsp',
                    'user', 'users', 'user presets'}
OUTPUT_FOLDER = 'Cab Disabled'


@dataclass(frozen=True, slots=True)
class Root:
    path: Path
    kind: str = 'Presets'
    product: str = ''  # Nonempty marks an explicitly browsed library boundary.


@dataclass(frozen=True, slots=True)
class Library:
    product: str
    path: Path
    sources: tuple[Source, ...]


@dataclass(frozen=True, slots=True)
class Discovery:
    libraries: tuple[Library, ...]
    issues: tuple[str, ...] = ()


def default_roots(platform=None, environ=None, home=None):
    platform = platform or sys.platform
    env = os.environ if environ is None else environ
    home = home or Path.home()
    if platform == 'win32':
        return (Root(Path(env['PROGRAMDATA']) / 'Neural DSP'),) if env.get('PROGRAMDATA') else ()
    if platform == 'darwin':
        return tuple(Root(base / 'Audio/Presets/Neural DSP') for base in (Path('/Library'), home / 'Library'))
    return ()


def category(path, library):
    parts = {p.casefold() for p in path.relative_to(library).parts[:-1]}
    if parts & {'user', 'users', 'user presets'}:
        return 'User'
    if parts & {'artist', 'artists'}:
        return 'Artist'
    if parts & {'factory', 'factory presets', 'neural dsp'}:
        return 'Factory'
    return 'Unknown'


def library_for(path, root):
    relative = path.relative_to(root)
    folders = relative.parts[:-1]
    if not folders or folders[0].casefold() in CATEGORY_FOLDERS:
        return root
    library = root / folders[0]
    if len(folders) > 1 and folders[1].casefold() == 'presets':
        library /= folders[1]
    return library


def discover(roots, max_depth=8, max_entries=50000, cancel=None):
    issues, files, groups = [], {}, {}
    remaining = max_entries
    for root in roots:
        if root.kind != 'Presets':
            continue
        base = absolute(root.path)
        try:
            if not base.exists():
                continue
            safe_path(base)
            if not base.is_dir():
                raise ValueError('Not a preset folder')
        except (OSError, ValueError) as exc:
            issues.append(f'{base}: {exc}');continue
        if base.name.casefold() == OUTPUT_FOLDER.casefold():
            continue
        stack, visited = [(base, 0)], set()
        while stack and remaining > 0 and not (cancel and cancel.is_set()):
            folder, depth = stack.pop()
            try:
                key = identity(folder, folder.stat())
                if key in visited:
                    continue
                visited.add(key)
                with os.scandir(folder) as entries:
                    for entry in entries:
                        if cancel and cancel.is_set():
                            break
                        remaining -= 1
                        if remaining < 0:
                            break
                        path = Path(entry.path)
                        if entry.is_symlink() or getattr(path, 'is_junction', lambda: False)():
                            continue
                        if entry.is_dir(follow_symlinks=False):
                            if path.name.casefold() == OUTPUT_FOLDER.casefold():
                                continue
                            if depth < max_depth:
                                stack.append((path, depth + 1))
                            else:
                                issues.append(f'{path}: scan depth limit')
                        elif path.suffix.lower() == '.xml' and entry.is_file(follow_symlinks=False):
                            library = base if root.product else library_for(path, base)
                            name = library.parent.name if library.name.casefold() == 'presets' else library.name
                            key = identity(path, entry.stat(follow_symlinks=False))
                            # Prefer the narrower association for overlapping browsed roots.
                            source = Source(path, name, library, category(path, library))
                            if key not in files or len(library.parts) > len(files[key].library.parts):
                                files[key] = source
            except OSError as exc:
                issues.append(f'{folder}: {exc}')
        if remaining <= 0:
            issues.append('Scan entry limit reached');break
    if cancel and cancel.is_set():
        issues.append('Discovery cancelled')
    for source in files.values():
        groups.setdefault((source.product, source.library), []).append(source)
    libraries = tuple(Library(product, path, tuple(sorted(sources, key=lambda s: str(s.path).casefold())))
                      for (product, path), sources in sorted(groups.items()))
    return Discovery(libraries, issues=tuple(issues))
