"""Offline private-pipe protocol, version 1. No sockets, GUI toolkit or RPC server.

One UTF-8 line per record, tab-separated. User-controlled strings are UTF-8 hex
encoded, so paths/newlines cannot inject records. STATE/CONTROL/OPTION/END is a
snapshot; STATUS/PROGRESS/ERROR/OPEN are incremental. Commands are an action plus
tab-separated key=hex(value) fields. See docs/ARCHITECTURE.md.
"""
import queue
import sys
import threading
import time

from .session import Session


def encode(value):
    return str(value).encode('utf-8').hex()


def command(line):
    parts = line.rstrip('\r\n').split('\t')
    if not parts[0].isascii() or not parts[0].replace('_', '').isalpha():
        raise ValueError('Invalid command')
    values = {}
    for field in parts[1:]:
        key, value = field.split('=', 1)
        if key in values:
            raise ValueError('Duplicate command field')
        values[key] = bytes.fromhex(value).decode('utf-8')
    return parts[0], values


def snapshot(view):
    lines = ['\t'.join(map(str, ('STATE', view['page'], int(view['busy']), int(view['back']),
             int(view['next']), encode(view['next_label']), encode(view['title']),
             encode(view['status']), encode(view['completion']), view['edited'], view['total'])))]
    for c in view['controls']:
        lines.append('\t'.join(('CONTROL', c['id'], c['kind'], str(c['height']),
                                encode(c['label']), encode(c['value']))))
        for option in c['options']:
            key, label, selected = option[:3]
            parent, kind, enabled, location = option[3:] if len(option) > 3 else (-1, '', True, '')
            lines.append('\t'.join(('OPTION', c['id'], encode(key), encode(label), str(int(selected)),
                                     str(parent), kind, str(int(enabled)), encode(location))))
    return '\n'.join(lines) + '\nEND\n'


def main():
    events = queue.Queue()
    session = Session(lambda *event: events.put(event))
    worker = None
    disconnected = False
    last_update = {'progress': 0.0, 'status': 0.0}

    def write(text):
        nonlocal disconnected
        if disconnected:
            return
        try:
            sys.stdout.write(text)
            sys.stdout.flush()
        except (BrokenPipeError, OSError):
            disconnected = True
            session.cancel.set()

    def read():
        while True:
            line = sys.stdin.readline(1024 * 1024 + 1)
            if not line:
                events.put(('eof',))
                return
            if len(line) > 1024 * 1024 or not line.endswith('\n'):
                events.put(('eof',))
                return
            events.put(('command', line))

    def run(kind, operation):
        try:
            result = operation()
            events.put(('finished', kind, result, None))
        except Exception as exc:
            events.put(('finished', kind, None, str(exc)))

    session.action('scan')
    write('HELLO\t2\n' + snapshot(session.view()))
    threading.Thread(target=read, daemon=True).start()
    while True:
        event = events.get()
        kind, *args = event
        if kind == 'eof':
            disconnected = True
            session.cancel.set()
            if not session.busy:
                break
        elif kind == 'command':
            try:
                name, values = command(args[0])
                if name == 'quit':
                    if session.busy:
                        session.action('cancel')
                    else:
                        break
                else:
                    session.action(name, values)
                write(snapshot(session.view()))
            except (ValueError, KeyError, IndexError, OSError) as exc:
                write('ERROR\t' + encode(exc) + '\n')
        elif kind == 'work':
            worker = threading.Thread(target=run, args=tuple(args), daemon=False)
            worker.start()
        elif kind == 'finished':
            session.finished(*args)
            if worker:
                worker.join()
                worker = None
            if disconnected:
                break
            write(snapshot(session.view()))
        elif kind == 'progress':
            if time.monotonic() - last_update[kind] >= 0.05:
                write(f'PROGRESS\t{args[0]}\t{args[1]}\n')
                last_update[kind] = time.monotonic()
        elif kind in ('error', 'status', 'authorization'):
            if kind != 'status' or time.monotonic() - last_update[kind] >= 0.05:
                write(('STATUS' if kind == 'authorization' else kind.upper()) + '\t' + encode(args[0]) + '\n')
                if kind == 'status':
                    last_update[kind] = time.monotonic()
    session.cancel.set()
    if worker:
        worker.join()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
