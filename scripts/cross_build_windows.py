"""Compile native Win32 frontend and C++ backend locally; no Python in the package."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from build_common import EXPAT_SOURCES, ROOT, VERSION, source_state, artifact_record, license_file
from package_windows import package_windows



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, default=Path('/workspace/tools/cab-cross'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--profile', choices=('prerelease', 'full'), default='prerelease')
    parser.add_argument('--gcc-lto', action='store_true', help='Experimental single-partition GCC LTO; avoids the pinned compiler partitioning ICE')
    args = parser.parse_args()
    tools, output = args.tools.resolve(), args.output.resolve()
    license_file("windows")
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, PATH=str(tools / 'root/usr/bin') + os.pathsep + os.environ.get('PATH', ''))
    cc = tools / 'root/usr/bin/x86_64-w64-mingw32-gcc-posix'
    cxx = tools / 'root/usr/bin/x86_64-w64-mingw32-g++-posix'
    windres = tools / 'root/usr/bin/x86_64-w64-mingw32-windres'
    resource, helper = output / 'app-resource.o', output / 'privileged-mode.o'
    subprocess.run([str(windres), '-I', str(ROOT / 'frontends/windows'), str(ROOT / 'frontends/windows/app.rc'), str(resource)], env=env, check=True)
    source = str(ROOT / 'frontends/windows/authorization.c')
    lto = ['-flto=auto', '-flto-partition=one'] if args.gcc_lto else []
    common = [str(cc), *lto, '-std=c11', '-Oz', '-s', '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra', '-static-libgcc']
    subprocess.run([*common, '-DCAB_PRIVILEGED_HELPER', '-c', source, '-o', str(helper)], env=env, check=True)
    auth = output / 'authorization.o'
    subprocess.run([*common, '-DCAB_WINDOWS_STATIC_AUTH', '-c', source, '-o', str(auth)], env=env, check=True)
    objects = []
    expat = ROOT / 'third_party/expat'
    for name in (*EXPAT_SOURCES, 'random_rand_s'):
        obj = output / (name + '.o')
        subprocess.run([*common, '-DHAVE_EXPAT_CONFIG_H', '-DXML_STATIC', '-I', str(expat),
                        '-c', str(expat / (name + '.c')), '-o', str(obj)], env=env, check=True)
        objects.append(obj)
    sources = sorted(p for p in (ROOT / 'native/backend').glob('*.cpp') if p.name != 'main.cpp')
    subprocess.run([str(cxx), *lto, '-std=c++17', '-Oz', '-s', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-municode', '-mwindows', '-static',
                    '-static-libgcc', '-static-libstdc++', '-D_WIN32_WINNT=0x0A00',
                    '-DCAB_WINDOWS_STATIC_AUTH', '-DCAB_WINDOWS_SINGLE_EXE', '-DXML_STATIC', '-I', str(expat),
                    str(ROOT / 'frontends/windows/main.cpp'), *map(str, sources),
                    str(helper), str(auth), str(resource), *map(str, objects),
                    '-o', str(output / 'Archetype Cab Disabler.exe'), '-lole32', '-lshell32', '-luuid',
                    '-lcomctl32', '-luxtheme', '-luser32', '-lgdi32', '-ladvapi32', '-lbcrypt'], env=env, check=True)
    for obj in [resource, helper, auth, *objects]: obj.unlink()
    version = VERSION
    archive = package_windows(output, output / f'ArchetypeCabDisabler-{version}-windows-x64.zip')
    metadata = dict(version=version, frontend='native Win32', backend='native C++17; no Python/Tk runtime',
                    **source_state(),
                    release_ready=False, frontend_execution='unverified', uac_execution='unverified',
                    build_host='Linux cloud; MinGW', validation_profile=args.profile,
                    **artifact_record(archive),
                    optimization=dict(compiler='-Oz',lto='single-partition (experimental)' if args.gcc_lto else 'off',lto_flags=lto,symbols='stripped at link',zip_level=9),
                    dependencies='Windows system APIs; statically linked C++/Expat', checks={'archive': 'CRC'})
    (output / 'build-info.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata, indent=2))


if __name__ == '__main__':
    main()
