"""Shared installer-style workflow. Native views never write preset files."""
from pathlib import Path
import threading
from ..backup import default_backup_path, desktop_directory
from ..discovery import Root, default_roots, discover, CATEGORY_FOLDERS
from ..engine import preview, apply
from .selection import tree
from ..authorization import provider_factory

PAGES = ('Select Plugins', 'Select presets', 'Backup', 'Edit mode', 'Review', 'Results')


class Session:
    def __init__(self, notify=lambda *args: None):
        self.notify = notify
        self.roots = list(default_roots())
        self.discovery = self.plan = None
        self.selected = set()
        self.selected_files = set()
        self.initialized = set()
        self.nodes = []
        self.page = 0
        self.busy = False
        self.cancel = threading.Event()
        self.backup = True
        self.backup_path = str(default_backup_path())
        self.reviewed_backup = None
        self.operation = 'disable'
        self.direct_authorization = provider_factory()
        self.report = ''
        self.result_summary = ''
        self.result_details = ''
        self.status = ''
        self.edited = self.total = 0
        self.completion = 'Ready'

    def control(self, key, kind, label='', value='', height=30, options=()):
        return dict(id=key, kind=kind, label=label, value=str(value), height=height, options=options)

    def view(self):
        c = self.control
        if self.page == 0:
            controls = [c('plugins', 'plugins', height=400, options=[
                (str(i), lib.product, lib.path in self.selected, -1, 'plugin', True, str(lib.path))
                for i, lib in enumerate(self.discovery.libraries if self.discovery else ())]),
                c('browse', 'buttons', label="Can't see your plugins?", options=[('add', 'Browse…', False)])]
        elif self.page == 1:
            controls = [c('presets', 'tree', height=440, options=[
                (n.id, n.display_label, bool(n.source and n.source.path in self.selected_files),
                 n.parent, 'preset' if n.source else 'folder', True, n.location) for n in self.nodes])]
        elif self.page == 2:
            controls = [c('backup', 'radio', 'Create backup?', str(int(self.backup)),
                options=[('1', 'Yes', self.backup), ('0', 'No', not self.backup)]),
                c('backup_path', 'readonly', 'Location', self.backup_path, height=24),
                c('save', 'buttons', options=[('save_backup', 'Browse…', False)])]
        elif self.page == 3:
            copying = self.operation in ('copy', 'copy-enable')
            disabling = self.operation in ('disable', 'copy')
            controls = [c('disable', 'pair', 'Cab simulation', str(int(disabling)), height=58,
                options=[('0', 'Enable', not disabling, -1, '', True, 'Enable cab simulation in the selected presets.'),
                         ('1', 'Disable', disabling, -1, '', True, 'Disable cab simulation in the selected presets.')]),
                c('copy', 'pair', 'Edit mode', str(int(copying)), height=100,
                options=[('0', 'Direct Edit', not copying, -1, '', True, 'Modify the saved presets directly, maintaining their names and locations within the preset browser. Direct edit may require elevated permissions.'),
                         ('1', 'Copy and Edit', copying, -1, '', True, 'Keep original presets, create and modify copies in separate "Cab Disabled" folder.')])]
        elif self.page == 4:
            controls = [c('report', 'text', value=self.report, height=410)]
            if not self.backup and not self.busy:
                controls.append(c('backup_notice', 'notice', value=(
                    'Backup not enabled!\n'
                    'By continuing I assume all responsibility for any loss or damage to presets.'), height=48))
        else:
            controls = [c('summary', 'readonly', value=self.result_summary, height=44),
                        c('details', 'text', label='Errors', value=self.result_details, height=350)]
        return dict(page=self.page, title=PAGES[self.page], busy=self.busy,
                    back=self.page not in (0, 5) and not self.busy,
                    next=not self.busy, next_label='Apply' if self.page == 4 else 'Finish' if self.page == 5 else 'Next',
                    status=self.status, completion=self.completion, edited=self.edited, total=self.total, controls=controls)

    @staticmethod
    def indices(value, count):
        values = value.split(',')
        if any(not n.isascii() or not n.isdecimal() or int(n) >= count for n in values if n):
            raise ValueError('Invalid selection')
        return {int(n) for n in values if n}

    def fields(self, values):
        if self.page == 0 and 'plugins' in values:
            libraries = self.discovery.libraries if self.discovery else ()
            self.selected = {libraries[i].path for i in self.indices(values['plugins'], len(libraries))}
        elif self.page == 1 and 'presets' in values:
            indices = self.indices(values['presets'], len(self.nodes))
            if any(self.nodes[i].source is None for i in indices):
                raise ValueError('Preset selection must contain files')
            visible = {n.source.path for n in self.nodes if n.source}
            self.selected_files.difference_update(visible)
            self.selected_files.update(self.nodes[i].source.path for i in indices)
        elif self.page == 2:
            backup = values.get('backup', str(int(self.backup)))
            if backup not in ('0', '1'):
                raise ValueError('Choose Yes or No for backup')
            self.backup = backup == '1'
            self.backup_path = values.get('backup_path', self.backup_path)
        elif self.page == 3:
            operation = values.get('operation', self.operation)
            if 'disable' in values or 'copy' in values:
                disabling = values.get('disable', str(int(self.operation in ('disable', 'copy'))))
                copying = values.get('copy', str(int(self.operation in ('copy', 'copy-enable'))))
                if disabling not in ('0', '1') or copying not in ('0', '1'):
                    raise ValueError('Invalid edit options')
                operation = ('copy' if disabling == '1' else 'copy-enable') if copying == '1' else ('disable' if disabling == '1' else 'enable')
            if operation not in ('disable', 'enable', 'copy', 'copy-enable'):
                raise ValueError('Invalid edit mode')
            self.operation = operation

    def action(self, name, values=None):
        values = values or {}
        if name == 'cancel':
            self.cancel.set()
            self.status = 'Cancelling...'
            self.notify('status', self.status)
            return
        if self.busy:
            raise ValueError('Wait for the current operation to finish')
        self.fields(values)
        if name == 'back':
            if self.page not in (0, 5):
                self.page -= 1
        elif name == 'add':
            if self.page != 0:
                raise ValueError('Return to Preset folders')
            path = Path(values['path']).expanduser().absolute()
            if not path.is_dir():
                raise ValueError('Choose a preset folder')
            if path.name.casefold() in CATEGORY_FOLDERS - {'neural dsp'}:
                path = path.parent
            # Browse can select a plugin's preset root or the Neural DSP collection.
            collection = path.name.casefold() == 'neural dsp'
            if not collection and (path / 'Presets').is_dir():
                path = path / 'Presets'
            root = Root(path, product='' if collection else path.name)
            if root not in self.roots:
                self.roots.append(root)
            self.start('scan', lambda: discover(tuple(self.roots), cancel=self.cancel))
        elif name == 'scan':
            if self.page != 0:
                raise ValueError('Return to Preset folders')
            self.start('scan', lambda: discover(tuple(self.roots), cancel=self.cancel))
        elif name == 'save_backup':
            if self.page != 2:
                raise ValueError('Return to Backup')
            if not self.backup:
                raise ValueError('Select Yes to choose a backup location')
            self.backup_path = values['path']
        elif name == 'next':
            self.next()
        else:
            raise ValueError('Unknown action')

    def next(self):
        if self.page == 0:
            if self.discovery is None:
                self.start('scan', lambda: discover(tuple(self.roots), cancel=self.cancel))
                return
            if not self.selected:
                raise ValueError('Select at least one plugin')
            for lib in self.discovery.libraries:
                if lib.path in self.selected and lib.path not in self.initialized:
                    self.selected_files.update(s.path for s in lib.sources if s.category != 'User')
                    self.initialized.add(lib.path)
            self.nodes = tree(self.discovery.libraries, self.selected)
            self.page = 1
        elif self.page == 1:
            if not any(n.source and n.source.path in self.selected_files for n in self.nodes):
                raise ValueError('Select at least one preset')
            self.page = 2
        elif self.page == 2:
            if self.backup:
                if not self.backup_path.strip():
                    raise ValueError('Choose a new .zip filename or select No')
                path = Path(self.backup_path).expanduser()
                if not path.is_absolute():
                    path = desktop_directory() / path
                if path.suffix.lower() != '.zip' or path.exists():
                    raise ValueError('Choose a new .zip filename')
                self.backup_path = str(path.absolute())
            self.page = 3
        elif self.page == 3:
            sources = tuple(n.source for n in self.nodes if n.source and n.source.path in self.selected_files)
            if not sources:
                raise ValueError('Select at least one preset')
            self.reviewed_backup = Path(self.backup_path) if self.backup else None
            self.start('review', lambda: preview(sources, self.operation, cancel=self.cancel, allow_elevation=self.direct_authorization is not None))
        elif self.page == 4:
            if not self.plan or not self.plan.complete or self.cancel.is_set():
                raise ValueError('Return to Edit mode and review again')
            self.edited, self.total = 0, len(self.plan.items)
            self.start('apply', lambda: apply(self.plan, self.cancel, self.progress,
                backup_path=self.reviewed_backup,
                backup_progress=lambda n, total: self.notify('status', f'Creating ZIP: {n}/{total}'),
                backup_created=lambda p: self.backup_created(p),
                direct_authorization=self.direct_authorization() if self.direct_authorization and self.operation in ('disable', 'enable') else None,
                authorization_notice=lambda text: self.notify('authorization', text)))
        else:
            raise ValueError('Finish closes the tool')

    def backup_created(self, path):
        self.report += '\nBackup: ' + str(path) + '\n'

    def progress(self, result):
        if result.status == 'changed':
            self.edited += 1
        self.notify('progress', self.edited, self.total)
        state = ('already enabled' if self.operation in ('enable', 'copy-enable') else 'already disabled') if result.status == 'already-correct' else 'File' if result.status == 'changed' else result.status
        self.notify('status', f'{state}: {result.source.stem}')

    def start(self, kind, operation):
        self.busy = True
        self.cancel.clear()
        self.completion = 'Working...'
        self.status = {'scan': 'Finding preset folders...', 'review': 'Preparing review...', 'apply': 'Editing presets...'}[kind]
        if kind != 'apply':
            self.edited = self.total = 0
        self.notify('work', kind, operation)

    def finished(self, kind, result=None, error=None):
        self.busy = False
        if error is not None:
            self.completion = 'Failed'
            self.status = str(error)
            self.notify('error', str(error))
            return
        self.completion = 'Ready'
        if kind == 'scan':
            self.discovery = result
            self.selected.intersection_update(lib.path for lib in result.libraries)
            self.status = ''
            self.plan = None
        elif kind == 'review':
            self.plan = result
            copying = result.operation in ('copy', 'copy-enable')
            lines = ['Edit mode: ' + ('Copy and Edit' if copying else 'Direct edit'),
                     'Backup: ' + ('Yes ' + str(self.reviewed_backup) if self.reviewed_backup else 'No'),
                     f'Presets: {len(result.items)}', '']
            lines += self.hierarchy({item.source: '' for item in result.items}, separate_plugins=True)
            self.report = '\n'.join(lines)
            self.page = 4
            self.status = ''
        elif kind == 'apply':
            successful = sum(r.status in ('changed', 'already-correct') for r in result)
            failed = len(result) - successful
            self.edited = sum(r.status == 'changed' for r in result)
            self.completion = 'Cancelled' if any(r.status == 'cancelled' for r in result) else 'Finished'
            self.result_summary = f'Successful: {successful}\nFailed: {failed}'
            errors = {r.source: self.relative_reason(r.reason) for r in result
                      if r.status not in ('changed', 'already-correct')}
            correct = {r.source: '' for r in result if r.status == 'already-correct'}
            lines = []
            if errors:
                lines += self.hierarchy(errors)
            if correct:
                if lines:
                    lines.append('')
                lines += ['Already enabled' if self.operation in ('enable', 'copy-enable') else 'Already disabled',
                          *self.hierarchy(correct)]
            self.result_details = '\n'.join(lines) if lines else 'No errors.'
            self.report = self.result_summary + '\n\n' + self.result_details
            self.page = 5
            self.status = ''

    def hierarchy(self, selected, separate_plugins=False):
        visible = set()
        for index, node in reversed(list(enumerate(self.nodes))):
            if node.source and node.source.path in selected or index in visible:
                visible.add(index)
                if node.parent >= 0:
                    visible.add(node.parent)
        depths, lines = [], []
        for index, node in enumerate(self.nodes):
            depth = depths[node.parent] + 1 if node.parent >= 0 else 0
            depths.append(depth)
            if index in visible:
                reason = selected.get(node.source.path, '') if node.source else ''
                if separate_plugins and node.parent < 0 and lines:
                    lines.append('')
                lines.append('    ' * depth + node.display_label + (' — ' + reason if reason else ''))
        return lines

    def relative_reason(self, reason):
        replacements = {n.location: n.label for n in self.nodes if n.location}
        replacements.update({str(n.source.path): n.display_label for n in self.nodes if n.source})
        if self.reviewed_backup:
            replacements[str(self.reviewed_backup)] = self.reviewed_backup.name
        for full, short in sorted(replacements.items(), key=lambda item: -len(item[0])):
            reason = reason.replace(full, short)
        return reason
