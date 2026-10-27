"""Syntax checks only: no object files, executables, app bundles or packages."""
import argparse
import ast
import json
import os
from pathlib import Path
import subprocess
from cross.macos_targets import TARGETS
from build_common import EXPAT_SOURCES

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, default=Path('/workspace/tools/cab-cross'))
    parser.add_argument('--report', type=Path)
    parser.add_argument('--target', choices=('all', 'macos', 'windows'), default='all',
                        help='Check only the frontend being changed; omit checks when a real compile will follow')
    args = parser.parse_args()
    tools = args.tools.resolve()
    files = [*ROOT.glob('*.py'), *ROOT.joinpath('scripts').rglob('*.py'), *ROOT.joinpath('tests').rglob('*.py')]
    for path in files:
        ast.parse(path.read_text(encoding='utf-8'), filename=str(path))
    commands = [[str(tools / 'root/usr/bin/x86_64-w64-mingw32-g++-posix'),
                 '-std=c++17', '-D_WIN32_WINNT=0x0A00', '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
                 '-DCAB_WINDOWS_STATIC_AUTH', '-DCAB_WINDOWS_SINGLE_EXE', '-DXML_STATIC', '-I', str(ROOT / 'third_party/expat'),
                 str(ROOT / 'frontends/windows/main.cpp'),
                 *map(str, sorted(ROOT.joinpath('native/backend').glob('*.cpp'))),
                 *map(str, sorted(ROOT.joinpath('native/tests').glob('*.cpp')))]]
    if args.target == 'macos':
        commands.clear()
    else:
        for extra in ([], ['-DCAB_PRIVILEGED_HELPER'], ['-DCAB_WINDOWS_STATIC_AUTH']):
            commands.append([str(tools / 'root/usr/bin/x86_64-w64-mingw32-gcc-posix'),
                             '-std=c11', '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
                             *extra, str(ROOT / 'frontends/windows/authorization.c')])
        commands.append([str(tools / 'root/usr/bin/x86_64-w64-mingw32-gcc-posix'), '-fsyntax-only', '-DHAVE_EXPAT_CONFIG_H', '-DXML_STATIC', '-I', str(ROOT / 'third_party/expat'), *map(str, (ROOT / 'third_party/expat' / (name + '.c') for name in (*EXPAT_SOURCES, 'random_rand_s')))])
    for arch, minimum in (TARGETS.items() if args.target != 'windows' else ()):
        flags = ['--target=' + arch + '-apple-macos' + minimum, '-isysroot', str(tools / 'sdk/MacOSX13.3.sdk'),
                 '-fsyntax-only', '-Wall', '-Wextra', '-Werror']
        commands += [[str(tools / 'root/usr/bin/clang++-19'), *flags, '-std=c++17', '-fobjc-arc', '-fblocks',
                      '-Wunguarded-availability', str(ROOT / 'frontends/macos/main.mm')],
                     [str(tools / 'root/usr/bin/clang++-19'), *flags, '-std=c++17', '-DXML_STATIC', '-I', str(ROOT / 'third_party/expat'), *map(str, sorted(ROOT.joinpath('native/backend').glob('*.cpp'))), *map(str, sorted(ROOT.joinpath('native/tests').glob('*.cpp')))],
                     [str(tools / 'root/usr/bin/clang-19'), *flags, str(ROOT / 'frontends/macos/authorization.c')],
                     [str(tools / 'root/usr/bin/clang-19'), *flags, '-DCAB_PRIVILEGED_HELPER', str(ROOT / 'frontends/macos/authorization.c')]]
        commands.append([str(tools / 'root/usr/bin/clang-19'), *flags, '-DHAVE_EXPAT_CONFIG_H', '-DXML_STATIC', '-I', str(ROOT / 'third_party/expat'), *map(str, (ROOT / 'third_party/expat' / (name + '.c') for name in EXPAT_SOURCES))])
    env = dict(os.environ, LD_LIBRARY_PATH=str(tools / 'root/usr/lib/x86_64-linux-gnu'))
    for command in commands:
        subprocess.run(command, env=env, check=True)
    report = dict(python_sources=len(files), syntax_commands=commands, emitted_build_artifacts=False,
                  native_execution='not performed', packaging='not performed')
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'{len(files)} Python sources and {len(commands)} native syntax checks passed; no builds produced')


if __name__ == '__main__':
    main()
