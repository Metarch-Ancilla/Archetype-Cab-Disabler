"""Build an opt-in Windows diagnostic; production source and app builds are untouched."""
import argparse
import json
from pathlib import Path
import subprocess
import zipfile
from build_common import ROOT, source_state, artifact_record, license_file


def instrument(source, name, stage):
    begin = source.index(name + '(')
    brace = source.index('{', begin) + 1
    return source[:brace] + f'\n    prof::Scope profileScope(prof::{stage});' + source[brace:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tools', type=Path, default=Path('/workspace/tools/cab-cross'))
    args = parser.parse_args();out = args.output.resolve();out.mkdir(parents=True, exist_ok=False)
    tools = args.tools.resolve() / 'root/usr/bin';source = ROOT / 'native/backend'
    platform = (source / 'platform.cpp').read_text().replace('#include "core.hpp"', '#include "core.hpp"\n#include "windows_apply_profile.hpp"', 1)
    for name, stage in (('readHandle', 'Read'), ('writeAll', 'Write'), ('sync', 'Flush'),
                        ('truncate', 'Truncate'), ('readSnapshot', 'Snapshot'), ('sha256', 'Hash'),
                        ('writeOriginal', 'Direct'), ('seek', 'Seek'), ('Authorization::authorize', 'Authorization'),
                        ('Authorization::open', 'AuthorizedOpen'), ('Authorization::~Authorization', 'AuthorizationClose')):
        platform = instrument(platform, name, stage)
    for text, stage in (('Signature signature(Handle h) {', 'Signature'), ('static Signature pathSignature(const PreparedPath &p) {', 'Metadata'), ('File::~File() {', 'Close')):
        assert text in platform;platform = platform.replace(text, text + f'\n    prof::Scope profileScope(prof::{stage});', 1)
    platform = platform.replace('static File openFile(const PreparedPath &p, bool writable, bool create, Signature *stamp=nullptr) {',
                                'static File openFile(const PreparedPath &p, bool writable, bool create, Signature *stamp=nullptr) {\n    prof::Scope profileScope(writable?prof::OpenWrite:prof::OpenRead);', 1)
    # Emulate the protected-file route on disposable, user-owned copies only.
    text = '    p.check();\n#ifdef _WIN32\n    HANDLE h ='
    assert text in platform
    platform = platform.replace(text, '    p.check();\n#ifdef _WIN32\n    if(prof::enabled&&prof::forceBroker&&writable)throw IOError("Diagnostic access-denied simulation",ERROR_ACCESS_DENIED);\n    HANDLE h =', 1)
    platform = platform.replace('join(bridgeDirectory(), "Archetype Cab Disabler.exe")', 'executable()')
    platform = platform.replace('    size_t offset = 0;\n    while (offset < data.size())', '    if(prof::enabled)prof::writeBytes+=data.size();\n    size_t offset = 0;\n    while (offset < data.size())', 1)
    platform = platform.replace('        out.append(block, (size_t)n);', '        if(prof::enabled)prof::readBytes+=(size_t)n;\n        out.append(block, (size_t)n);', 1)
    marker='    void check() const {'
    assert marker in platform
    platform=platform.replace(marker,marker+'\n        prof::Scope profileScope(prof::PathCheck);',1)
    platform=platform.replace('        for(const auto &ancestor:ancestors) {',
                              '        for(const auto &ancestor:ancestors) {\n            if(prof::enabled)++prof::pathComponents;',1)
    text = '    require(GetFileType(handle(h)) == FILE_TYPE_DISK && GetFileInformationByHandle(handle(h), &i),\n            "Not a regular disk file");'
    assert text in platform
    platform = platform.replace(text, '    DWORD kind;BOOL valid=FALSE;\n    {prof::Scope timer(prof::FileType);kind=GetFileType(handle(h));}\n    if(kind==FILE_TYPE_DISK){prof::Scope timer(prof::FileInfo);valid=GetFileInformationByHandle(handle(h),&i);}\n    require(kind==FILE_TYPE_DISK&&valid,"Not a regular disk file");', 1)
    (out / 'platform.cpp').write_text(platform)
    engine = (source / 'engine.cpp').read_text().replace('#include "core.hpp"', '#include "core.hpp"\n#include "windows_apply_profile.hpp"', 1)
    engine = instrument(engine, 'apply', 'Apply').replace('auto probe = openWrite(i.source.path);', 'prof::Scope probeScope(prof::Preflight);auto probe = openWrite(i.source.path);')
    engine = engine.replace('    if (needs) {', '    if(prof::enabled&&prof::forceBroker)needs=true;\n    if (needs) {', 1)
    (out / 'engine.cpp').write_text(engine)
    bridge = (ROOT / 'frontends/windows/authorization.c').read_text()
    bridge = 'extern void profile_shell(int begin);\n' + bridge
    bridge = bridge.replace('    BOOL launched=ShellExecuteExW(&execute);DWORD launch_error=GetLastError();', '    profile_shell(1);BOOL launched=ShellExecuteExW(&execute);DWORD launch_error=GetLastError();profile_shell(0);', 1)
    (out / 'authorization.c').write_text(bridge)
    manifest = ROOT / 'frontends/windows/app.manifest';notices = license_file('windows')
    (out / 'diagnostic.rc').write_text(f'1 24 "{manifest}"\n102 RCDATA "{notices}"\n')
    includes = ['-I', str(source), '-I', str(ROOT / 'third_party/expat'), '-I', str(ROOT / 'frontends/windows'), '-I', str(ROOT / 'diagnostics')]
    objects = []
    cc = str(tools / 'x86_64-w64-mingw32-gcc-posix');cxx = str(tools / 'x86_64-w64-mingw32-g++-posix')
    def call(*command): subprocess.run(list(command), check=True)
    for name in ('xmlparse', 'xmlrole', 'xmltok'):
        obj = out / (name + '.o');call(cc, '-Oz', '-DXML_STATIC', '-DHAVE_EXPAT_CONFIG_H', *includes, '-c', str(ROOT / 'third_party/expat' / (name + '.c')), '-o', str(obj));objects.append(str(obj))
    for name, macro in (('bridge', '-DCAB_WINDOWS_STATIC_AUTH'), ('helper', '-DCAB_PRIVILEGED_HELPER')):
        obj = out / (name + '.o');call(cc, '-Oz', '-std=c11', '-Wall', '-Wextra', '-Werror', macro, *includes, '-c', str(out / 'authorization.c'), '-o', str(obj));objects.append(str(obj))
    obj = out / 'resource.o';call(str(tools / 'x86_64-w64-mingw32-windres'), str(out / 'diagnostic.rc'), str(obj));objects.append(str(obj))
    state = source_state();exe = out / 'WindowsApplyProfiler.exe'
    call(cxx, '-std=c++17', '-Oz', '-s', '-static', '-static-libgcc', '-static-libstdc++', '-municode', '-D_WIN32_WINNT=0x0A00', '-DCAB_WINDOWS_STATIC_AUTH', '-DCAB_WINDOWS_SINGLE_EXE', '-DXML_STATIC', f'-DCAB_PROFILE_COMMIT="{state["source_commit"]}"', *includes,
         str(ROOT / 'diagnostics/windows_apply_profile.cpp'), str(out / 'platform.cpp'), str(out / 'engine.cpp'), *[str(source / name) for name in ('formats.cpp', 'backup.cpp', 'discovery.cpp')], *objects,
         '-lbcrypt', '-lshell32', '-lole32', '-luuid', '-ladvapi32', '-o', str(exe))
    archive = out / 'WindowsApplyProfiler.zip'
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED, compresslevel=9) as stream: stream.write(exe, exe.name)
    with zipfile.ZipFile(archive) as stream: assert stream.testzip() is None and stream.namelist() == [exe.name]
    (out / 'build-info.json').write_text(json.dumps(dict(**state, **artifact_record(archive), diagnostic=True, native_execution='unverified'), indent=2))
    print(archive)


if __name__ == '__main__':
    main()
