"""Immutable previews and verified direct writes and no-clobber copies; no history database."""
from dataclasses import dataclass
from pathlib import Path
import os
import tempfile
from threading import Event
from typing import Callable
from .adapters import PRODUCTION, Registry, Unsupported, representation, validate_inspection
from .files import absolute, digest, identity, read_preset, read_snapshot, safe_path, signature, write_original

MAX_PLAN_BYTES = 64 * 1024 * 1024


@dataclass(frozen=True, slots=True)
class Source:
    path: Path
    product: str
    library: Path
    category: str = 'Unknown'


@dataclass(frozen=True, slots=True)
class Item:
    source: Path
    destination: Path | None
    before_hash: str
    output: bytes | None
    status: str
    reason: str
    before: tuple[bool, ...] = ()
    after: tuple[bool, ...] = ()
    adapter: str = ''
    source_signature: tuple[int, ...] = ()


@dataclass(frozen=True, slots=True)
class Plan:
    operation: str
    items: tuple[Item, ...]
    warnings: tuple[str, ...] = ()
    complete: bool = True


@dataclass(frozen=True, slots=True)
class Outcome:
    source: Path
    status: str
    reason: str


def preview(sources: tuple[Source, ...], operation: str = 'disable',
            destination: Path | None = None, registry: Registry = PRODUCTION,
            cancel: Event | None = None, allow_elevation: bool = False) -> Plan:
    if operation not in ('copy', 'copy-enable', 'disable', 'enable'):
        raise ValueError('Unknown operation')
    items = []
    seen = set()
    targets = set()
    planned_bytes = 0
    enabled = operation in ('enable', 'copy-enable')
    for source in sources:
        if cancel and cancel.is_set():
            break
        path = absolute(source.path)
        data = None
        try:
            safe_path(path)
            key = identity(path)
            if key in seen:
                continue
            seen.add(key)
            data, source_signature = read_snapshot(path)
            before_hash = digest(data)
            adapter, info = registry.identify(data, source.product)
            # Built-ins can reuse the just-validated immutable inspection. The
            # output still undergoes full independent parsing and gap checks.
            patch_validated = getattr(adapter, '_patch_validated', None)
            output = (patch_validated(data, enabled, info) if patch_validated
                      else adapter.patch(data, enabled))
            check = adapter.inspect(output)
            validate_inspection(check, len(output), adapter.id)
            if check.enabled != (enabled,) * len(info.enabled):
                raise Unsupported('Patched cab state did not validate')
            if (check.adapter, check.product, check.version, check.schema, check.display_name) != (
                    info.adapter, info.product, info.version, info.schema, info.display_name):
                raise Unsupported('Adapter changed identity, schema or display metadata')
            # String true/false encodings differ in length. Compare every untouched
            # gap and suffix at their corresponding offsets, not absolute offsets.
            if len(info.cab_spans) != len(check.cab_spans):
                raise Unsupported('Adapter changed cab structure')
            previous_before = previous_after = 0
            for (a, b), (c, d) in zip(sorted(info.cab_spans), sorted(check.cab_spans)):
                if c < previous_after or d <= c or d > len(output):
                    raise Unsupported('Invalid patched scalar spans')
                if data[previous_before:a] != output[previous_after:c]:
                    raise Unsupported('Adapter changed bytes outside validated cab spans')
                previous_before, previous_after = b, d
            if data[previous_before:] != output[previous_after:]:
                raise Unsupported('Adapter changed bytes outside validated cab spans')
            target = path
            if operation in ('copy', 'copy-enable'):
                relative = path.relative_to(absolute(source.library))
                if any(part.casefold() == 'cab disabled' for part in relative.parts[:-1]):
                    raise Unsupported('Generated copies cannot be copied recursively')
                if destination is None:
                    folder = absolute(source.library) / 'Cab Disabled'
                else:
                    product = source.product
                    if not product or product in ('.', '..') or any(c in product for c in '/\\:'):
                        raise Unsupported('Unsafe product folder name')
                    folder = absolute(destination) / product
                target = folder / relative
                safe_path(target)
                if target == path or (target.exists() and os.path.samefile(target, path)):
                    raise Unsupported('Copy destination aliases source')
                # Latest user requirement: flag only. Even copies preserve the
                # embedded name, UID, path and category bytes without rewriting.
            target_key = os.path.normcase(str(target.resolve()))
            if target_key in targets:
                raise Unsupported('Multiple sources map to the same destination')
            targets.add(target_key)
            status, reason = 'ready', 'Validated cab scalar change'
            if operation in ('copy', 'copy-enable') and target.exists():
                if read_preset(target) == output:
                    status, reason = 'already-correct', 'Identical copy already exists'
                else:
                    status, reason = 'skipped', 'Destination collision; choose another destination'
            elif operation not in ('copy', 'copy-enable') and output == data:
                status, reason = 'already-correct', 'Cab section already in requested state'
            parent = target.parent
            while not parent.exists() and parent != parent.parent:
                parent = parent.parent
            if (status == 'ready' and not os.access(parent if operation in ('copy', 'copy-enable') else path, os.W_OK)
                    and not (allow_elevation and operation in ('disable', 'enable'))):
                status, reason = 'failed', 'Write access denied; no automatic elevation'
            if status == 'ready':
                if planned_bytes + len(output) > MAX_PLAN_BYTES:
                    raise Unsupported('64 MiB plan limit exceeded; select a smaller batch')
                planned_bytes += len(output)
            else:
                output = None
            items.append(Item(path, target, before_hash, output, status, reason,
                              info.enabled, (enabled,) * len(info.enabled), adapter.id, source_signature))
        except (OSError, ValueError) as exc:
            kind = representation(data) + ': ' if data is not None and isinstance(exc, Unsupported) else ''
            status = 'failed' if isinstance(exc, OSError) else 'skipped'
            items.append(Item(path, None, '', None, status, kind + str(exc)))
    warnings = ('Close plugins/DAWs and other preset writers before Apply. '
                'Factory updates may replace in-place edits.',
                'Enable is stateless: it enables current cab sections and cannot restore historical states.')
    if cancel and cancel.is_set():
        warnings += ('Preview cancelled; run Review again before applying.',)
    return Plan(operation, tuple(items), warnings, not (cancel and cancel.is_set()))


def apply(plan: Plan, cancel: Event | None = None,
          progress: Callable[[Outcome], None] | None = None,
          backup_path: Path | None = None, backup_progress=None, backup_created=None, direct_authorization=None, authorization_notice=None) -> tuple[Outcome, ...]:
    if not plan.complete:
        outcomes = tuple(Outcome(item.source, 'cancelled', 'Preview incomplete; review again; no presets edited')
                         for item in plan.items)
        if progress:
            for outcome in outcomes:
                progress(outcome)
        return outcomes
    if backup_path is not None:
        from .backup import BackupCancelled, create_backup
        try:
            saved = create_backup(plan, backup_path, cancel, backup_progress)
            if saved is None and any(item.status == 'ready' for item in plan.items):
                raise Unsupported('Backup verification did not complete; no presets edited')
            if saved is not None and backup_created:
                backup_created(saved)
        except (OSError, ValueError, RuntimeError) as exc:
            status = 'cancelled' if isinstance(exc, BackupCancelled) else 'failed'
            outcomes = tuple(Outcome(item.source, status if item.status == 'ready' else item.status,
                                     f'Backup stopped batch; no presets edited: {exc}'
                                     if item.status == 'ready' else item.reason) for item in plan.items)
            if progress:
                for outcome in outcomes:
                    progress(outcome)
            return outcomes
    if direct_authorization is None or plan.operation not in ('disable', 'enable') or not any(i.status == 'ready' for i in plan.items):
        return _apply_items(plan, cancel, progress)
    from .authorization import requires_authorization
    selected_paths = tuple(i.source for i in plan.items if i.status == 'ready')
    if not requires_authorization(selected_paths):
        return _apply_items(plan, cancel, progress)
    try:
        if cancel and cancel.is_set():
            from .authorization import AuthorizationCancelled
            raise AuthorizationCancelled('Cancelled before administrator authorisation; no presets edited')
        if authorization_notice:
            authorization_notice('Direct edit requires elevated permissions')
        direct_authorization.authorize(selected_paths)
    except (OSError, ValueError) as exc:
        from .authorization import AuthorizationCancelled
        status = 'cancelled' if isinstance(exc, AuthorizationCancelled) else 'failed'
        outcomes = tuple(Outcome(i.source, status if i.status == 'ready' else i.status,
                                 str(exc) if i.status == 'ready' else i.reason) for i in plan.items)
        if progress:
            for outcome in outcomes:
                progress(outcome)
        return outcomes
    try:
        return _apply_items(plan, cancel, progress, opener=direct_authorization.open)
    finally:
        direct_authorization.close()


def _apply_items(plan, cancel, progress, opener=None):
    results = []
    for item in plan.items:
        if cancel and cancel.is_set():
            result = Outcome(item.source, 'cancelled', 'Not written; completed files remain valid')
        elif item.status != 'ready':
            result = Outcome(item.source, item.status, item.reason)
        else:
            temporary = None
            result = Outcome(item.source, 'failed', 'Write interrupted')
            try:
                if item.output is None or item.destination is None:
                    raise Unsupported('Incomplete plan')
                safe_path(item.source)
                if (signature(item.source) != item.source_signature
                        or digest(read_preset(item.source)) != item.before_hash):
                    raise Unsupported('Source changed since preview; review again')
                target = item.destination
                safe_path(target)
                if plan.operation in ('copy', 'copy-enable'):
                    if target.exists():
                        raise Unsupported('Destination appeared since preview; no overwrite')
                    target.parent.mkdir(parents=True, exist_ok=True)
                    safe_path(target)
                    fd, temporary = tempfile.mkstemp(prefix='.cab-', dir=target.parent)
                    with os.fdopen(fd, 'wb') as f:
                        f.write(item.output)
                        f.flush()
                        os.fsync(f.fileno())
                    temporary = Path(temporary)
                    if read_preset(temporary) != item.output:
                        raise Unsupported('Temporary output verification failed')
                    safe_path(target)
                    if (signature(item.source) != item.source_signature
                            or digest(read_preset(item.source)) != item.before_hash):
                        raise Unsupported('Source changed during write; review again')
                    # Atomic no-clobber publication remains exclusive to copies.
                    os.link(temporary, target)
                else:
                    write_original(target, item.output, item.source_signature, item.before_hash, opener=opener)
                result = Outcome(item.source, 'changed', str(target))
            except (OSError, ValueError) as exc:
                result = Outcome(item.source, 'failed', str(exc))
            finally:
                if temporary is not None:
                    try:
                        Path(temporary).unlink(missing_ok=True)
                    except OSError as exc:
                        result = Outcome(result.source, result.status, result.reason + f'; temporary cleanup failed: {exc}')
        results.append(result)
        if progress:
            progress(result)
    return tuple(results)
