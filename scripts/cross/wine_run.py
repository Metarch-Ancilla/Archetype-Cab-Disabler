"""Run Windows build/check commands in the prepared, workspace-local Wine runtime."""
import argparse
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
import time


def windows_path(path):
    return 'Z:' + str(Path(path).resolve()).replace('/', '\\')


def run(arguments, tools, *, timeout=None):
    tools = Path(tools).resolve()
    env = dict(os.environ, LD_LIBRARY_PATH=str(tools / 'root/usr/lib/x86_64-linux-gnu'),
               WINEPREFIX=str(tools / 'wine-prefix-display'), WINEDEBUG='-all',
               WINEDLLOVERRIDES='mscoree,mshtml=',
               CABDISABLER_BUILD_CONTEXT='Linux cloud using Wine 10.0; native Windows execution unverified',
               CABDISABLER_SOURCE_COMMIT=subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
               CABDISABLER_SOURCE_DIRTY=str(bool(subprocess.check_output(['git', 'status', '--porcelain']))),
               XDG_CACHE_HOME=str(tools / 'cache'))
    display = None
    with tempfile.TemporaryFile() as log:
        if not env.get('DISPLAY'):
            xvfb = tools.parent / 'xvfb/root/usr/bin/Xvfb'
            if not xvfb.is_file():
                xvfb = tools / 'root/usr/bin/Xvfb'
            if not xvfb.is_file():
                raise RuntimeError('Xvfb is required to initialize Wine and check native windows')
            for number in range(98, 198):
                if not Path(f'/tmp/.X11-unix/X{number}').exists() and not Path(f'/tmp/.X{number}-lock').exists():
                    break
            else:
                raise RuntimeError('No free virtual display')
            env['DISPLAY'] = f':{number}'
            display = subprocess.Popen([str(xvfb), env['DISPLAY'], '-screen', '0',
                                        '1280x900x24', '-nolisten', 'tcp'], stdout=log, stderr=log)
        try:
            if display:
                deadline = time.monotonic() + 5
                while not Path(f'/tmp/.X11-unix/X{number}').exists():
                    if display.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError('Virtual display did not start')
                    time.sleep(.05)
            wine = str(tools / 'root/usr/lib/wine/wine64')
            prefix = tools / 'wine-prefix-display'
            if not (prefix / 'drive_c/windows/system32/kernel32.dll').exists():
                subprocess.run([wine, str(tools / 'windows-python/python.exe'), '-c', 'pass'],
                               env=env, check=True, timeout=45)
            zlib = prefix / 'drive_c/windows/system32/zlib1.dll'
            if not zlib.exists():
                shutil.copy2(tools / 'root/usr/x86_64-w64-mingw32/lib/zlib1.dll', zlib)
            return subprocess.run([wine, *arguments],
                                  env=env, check=True, timeout=timeout)
        finally:
            if display:
                display.terminate()
                display.wait(timeout=5)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, default=Path('/workspace/tools/cab-cross'))
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    arguments = args.arguments
    if arguments and arguments[0] == '--':
        arguments = arguments[1:]
    if not arguments:
        parser.error('Provide a Windows executable and arguments after --')
    run(arguments, args.tools)
