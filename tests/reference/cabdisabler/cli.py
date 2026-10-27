"""A small offline command line; all mutations require an explicit --apply."""
import argparse
from collections import Counter
import json
from pathlib import Path
from .adapters import PRODUCTION, representation
from .discovery import Root, default_roots, discover
from .engine import Source, preview, apply, read_preset
from .errors import Unsupported


def main(argv=None):
    parser = argparse.ArgumentParser(description='Offline cab-flag editing with automatic preset format detection')
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('adapters', help='List supported preset format readers')
    scan = sub.add_parser('scan')
    scan.add_argument('--preset-root', type=Path, action='append', default=[])
    inspect = sub.add_parser('inspect')
    inspect.add_argument('files', type=Path, nargs='+')
    edit = sub.add_parser('edit')
    edit.add_argument('operation', choices=['copy', 'disable', 'enable'])
    edit.add_argument('files', type=Path, nargs='+')
    edit.add_argument('--product', help='Optional library label; format detection uses file contents')
    edit.add_argument('--library', type=Path, help='Optional source library root')
    edit.add_argument('--destination', type=Path)
    edit.add_argument('--apply', action='store_true')
    edit.add_argument('--backup', type=Path, help='Create and verify this ZIP before any edits; never overwrite')
    args = parser.parse_args(argv)
    if args.command == 'adapters':
        print(json.dumps([{'id': a.id, 'product': a.product, 'version': a.version, 'evidence': a.evidence}
                          for a in PRODUCTION.adapters], indent=2))
        return 0
    if args.command == 'scan':
        result = discover(default_roots() + tuple(Root(p, 'Presets') for p in args.preset_root))
        print(json.dumps({'libraries': [{'product': l.product, 'path': str(l.path),
              'count': len(l.sources), 'support': 'Format detected per file during review'}
              for l in result.libraries],
              'issues': result.issues}, indent=2, ensure_ascii=False))
        return 1 if result.issues else 0
    if args.command == 'inspect':
        failed = False
        for path in args.files:
            try:
                data = read_preset(path)
                matched = None
                reason = None
                try:
                    _adapter, info = PRODUCTION.identify(data)
                    matched = {'product': info.product, 'version': info.version,
                               'format': info.adapter, 'cab_enabled': info.enabled,
                               'support': 'structure validated; plugin loading unverified'}
                except Unsupported as exc:
                    reason = str(exc)
                print(json.dumps({'path': str(path), 'bytes': len(data), 'representation': representation(data),
                                  'adapter': matched, 'reason': reason, 'support': 'structure validated' if matched else 'unsupported / read-only'}, ensure_ascii=False))
            except (OSError, ValueError) as exc:
                print(json.dumps({'path': str(path), 'error': str(exc)}))
                failed = True
        return int(failed)
    try:
        plan = preview(tuple(Source(p, args.product or 'Auto-detected', args.library or p.parent) for p in args.files),
                       args.operation, args.destination)
    except ValueError as exc:
        parser.error(str(exc))
    if args.backup:
        print('Requested backup ZIP:', args.backup)
    for item in plan.items:
        print(f'{item.status}: {item.source} -> {item.destination or "untouched"}: {item.reason}')
    if args.apply:
        results = apply(plan, backup_path=args.backup,
                        backup_created=lambda path: print('Backup created and verified:', path))
        print(json.dumps(dict(Counter(r.status for r in results)), sort_keys=True))
        return 1 if any(r.status in ('skipped', 'failed', 'cancelled') for r in results) else 0
    return 1 if any(i.status in ('skipped', 'failed') for i in plan.items) else 0
