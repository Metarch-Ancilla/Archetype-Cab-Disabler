"""Assemble complete per-platform notices; no runtime/build tools are bundled."""
import argparse
from pathlib import Path
import re
from build_common import ROOT, VERSION


def section(title, text):
    return '\n' + '=' * 78 + '\n' + title + '\n' + '=' * 78 + '\n\n' + text.rstrip('\n') + '\n'


def common_notices():
    text = section('Application — MIT', (ROOT / 'LICENSE').read_text())
    text += section('Expat — MIT', (ROOT / 'third_party/expat/COPYING').read_text())
    # Preserve every verbatim copyright line, not just the older short COPYING.
    copyrights = {}
    for path in sorted((ROOT / 'third_party/expat').iterdir()):
        if path.suffix not in ('.h', '.c'):
            continue
        for line in path.read_text().splitlines():
            if re.match(r'\s*Copyright \(c\)', line):
                copyrights.setdefault(line.strip(), []).append(path.name)
    text += section('Additional Expat source copyright notices — MIT', '\n'.join(copyrights))
    siphash = (ROOT / 'third_party/expat/siphash.h').read_text()
    attribution = siphash[siphash.index(' * Derived by'):siphash.index(' * HISTORY:')]
    text += section('Expat bundled SipHash implementation — CC0-1.0', attribution)
    return text


def generate(tools, common=Path('/usr/share/common-licenses')):
    output = ROOT / 'licenses';output.mkdir(exist_ok=True)
    shared = common_notices() + section('CC0 1.0 Universal — complete text', (common / 'CC0-1.0').read_text())
    mac = ('Archetype Cab Disabler — macOS licenses and notices\n'
           f'Version: {VERSION}\n'
           'Build profile: macos-system-runtime\n\n'
           'Bundled: application, Expat and its SipHash implementation.\n'
           'AppKit, Foundation, Security and libc++ are supplied by macOS, not\n'
           'redistributed in this app. Compiler/SDK/build tools are not included.\n'
           'The libc++ header-code license and LLVM exception are retained below.\n') + shared + section('libc++ 15.0.6 headers — Apache-2.0 WITH LLVM-exception and upstream notices', (ROOT / 'licenses/inputs/libcxx-15.0.6-LICENSE.txt').read_text())
    windows = ('Archetype Cab Disabler — Windows licenses and notices\n'
           f'Version: {VERSION}\n'
               'Build profile: windows-mingw\n\n'
               'Bundled: application, Expat/SipHash and statically linked GCC/MinGW\n'
               'runtime code, including winpthreads where linked. The compiler,\n'
               'SDK, Python and build tools are not distributed. Windows system\n'
               'DLLs are supplied by Windows.\n\n'
               'The GCC Runtime Library Exception permits eligible compiled\n'
               'combinations under the application license; inclusion of GPL\n'
               'texts does not relicense the independent application as GPL.\n'
               'Complete pinned toolchain copyright records are retained below.\n'
               'They also describe unbundled tools, tests and source files; their\n'
               'inclusion is not a claim that every listed component is shipped.\n'
               'Referenced standard license texts are included as appendices,\n'
               'so recipients need no external common-licenses directory.\n'
               'This file does not cover the experimental Microsoft-runtime build.\n') + shared
    for package in ('gcc-mingw-w64-base', 'mingw-w64-common'):
        path = tools / 'root/usr/share/doc' / package / 'copyright'
        windows += section(package + ' — complete packaged copyright record', path.read_text())
    for name in ('GPL-2', 'GPL-3', 'LGPL-2', 'LGPL-2.1', 'LGPL-3', 'GFDL-1.2', 'Artistic'):
        windows += section(name + ' — complete referenced license text', (common / name).read_text())
    for platform, text in (('macos', mac), ('windows', windows)):
        (output / (platform + '-LICENSES.txt')).write_text(text, encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, default=Path('/workspace/tools/cab-cross'))
    args = parser.parse_args();generate(args.tools.resolve())
