"""Compile the C++ backend with the pinned native cross toolchains; no Python runtime."""
from pathlib import Path
import subprocess
import hashlib
import shutil
import tempfile
import os
from build_common import EXPAT_SOURCES, ROOT
from cross.macos_targets import TARGETS

BACKEND = sorted((ROOT / 'native/backend').glob('*.cpp'))
EXPAT = ROOT / 'third_party/expat'


def fingerprint(tools, target):
    inputs = [Path(__file__), Path(__file__).with_name("build_common.py"),
              *BACKEND, *sorted((ROOT / 'native/backend').glob('*.hpp')),
              *sorted((ROOT / 'shared').glob('*.h*')), *sorted(ROOT.joinpath('frontends/shared').glob('*.h*')),
              *sorted(EXPAT.glob('*.h')), *sorted(EXPAT.glob('*.c')),
              tools / 'debian-inputs.json']
    inputs += ([ROOT / 'native/backend/windows.rc', ROOT / 'frontends/windows/app.manifest', ROOT / 'frontends/windows/version.rcinc']
               if target == 'windows' else [tools / 'download-inputs.json', ROOT / 'scripts/cross/macos_targets.py'])
    stamp = hashlib.sha256((str(tools) + target).encode())
    for path in inputs:
        stamp.update(str(path).encode() + b'\0'); stamp.update(path.read_bytes())
    return stamp.hexdigest()


def publish_cache(source, cache):
    cache.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=cache.parent, delete=False) as stream: temporary = Path(stream.name)
    try:
        shutil.copy2(source, temporary); os.replace(temporary, cache)
    finally: temporary.unlink(missing_ok=True)


def backend(tools, output, target, env):
    output = Path(output)
    cache = ROOT / 'build/native-cache/cpp' / target / fingerprint(tools, target)
    if cache.is_file():
        shutil.copy2(cache, output)
        return
    work = output.parent / ('objects-' + target)
    work.mkdir(parents=True, exist_ok=True)
    if target == 'windows':
        cc = str(tools / 'root/usr/bin/x86_64-w64-mingw32-gcc-posix')
        cxx = str(tools / 'root/usr/bin/x86_64-w64-mingw32-g++-posix')
        flags = ['-Oz', '-D_WIN32_WINNT=0x0A00', '-ffunction-sections', '-fdata-sections']
        link = ['-s', '-municode', '-static', '-static-libgcc', '-static-libstdc++', '-Wl,--gc-sections',
                '-lbcrypt', '-lshell32', '-lole32', '-luuid']
    else:
        cc, cxx = (str(tools / ('root/usr/bin/' + name + '-19')) for name in ('clang', 'clang++'))
        minimum = TARGETS[target]
        flags = ['--target=' + target + '-apple-macos' + minimum, '-isysroot', str(tools / 'sdk/MacOSX13.3.sdk'),
                 '-Oz', '-flto=thin', '-fvisibility=hidden']
        link = ['--ld-path=' + str(tools / 'root/usr/bin/ld64.lld-19'), '-flto=thin', '-Wl,-dead_strip',
                '-Wl,-platform_version,macos,' + minimum + ',13.3']
    objects = []
    for name in EXPAT_SOURCES + (('random_rand_s',) if target == 'windows' else ()):
        obj = work / (name + '.o')
        subprocess.run([cc, *flags, '-DHAVE_EXPAT_CONFIG_H', '-DXML_STATIC', '-I', str(EXPAT),
                        '-c', str(EXPAT / (name + '.c')), '-o', str(obj)], env=env, check=True)
        objects.append(str(obj))
    if target == 'windows':
        resource = work / 'backend-resource.o'
        subprocess.run([str(tools / 'root/usr/bin/x86_64-w64-mingw32-windres'),
                        '-I', str(ROOT / 'native/backend'), str(ROOT / 'native/backend/windows.rc'), str(resource)],env=env,check=True)
        objects.append(str(resource))
    subprocess.run([cxx, *flags, '-std=c++17', '-DXML_STATIC', '-I', str(EXPAT),
                    *map(str, BACKEND), *objects, '-o', str(output), *link], env=env, check=True)
    if target != 'windows':
        subprocess.run([str(tools / 'root/usr/bin/llvm-strip-19'), '-x', str(output)], env=env, check=True)
    shutil.rmtree(work)
    publish_cache(output, cache)
