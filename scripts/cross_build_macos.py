"""Build a native C++/AppKit universal app locally; no Python runtime."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import shutil
import stat
import struct
import subprocess
import zipfile
from concurrent.futures import ThreadPoolExecutor
from macholib.MachO import MachO
from cross.verify_macos import verify_app
from cross.macos_targets import TARGETS, CPU_ARCHITECTURES, validate_deployment
from native_build import backend
from build_common import ROOT, VERSION, source_state, artifact_record, license_file
NAME='Archetype Cab Disabler'
MACH_MAGICS=(b'\xcf\xfa\xed\xfe',b'\xca\xfe\xba\xbe')


def macho_paths(root):
    for path in sorted(root.rglob('*')):
        if path.is_file() and not path.is_symlink() and path.suffix != '.a':
            with path.open('rb') as stream:
                if stream.read(4) in MACH_MAGICS:
                    yield path

def universal(output, slices):
    """Join thin executables with 16 KiB aligned slices (including Apple Silicon)."""
    records, offset = [], 16384
    for path in slices:
        content = path.read_bytes()
        header = MachO(str(path)).headers[0].header
        records.append((header.cputype, header.cpusubtype, offset, len(content), 14, content))
        offset = (offset + len(content) + 16383) & ~16383
    with output.open('xb') as stream:
        stream.write(struct.pack('>II', 0xcafebabe, len(records)))
        for cpu, subtype, offset, size, align, _ in records:
            stream.write(struct.pack('>IIIII', cpu, subtype, offset, size, align))
        for _, _, offset, _, _, content in records:
            stream.seek(offset)
            stream.write(content)
    output.chmod(0o755)

def inventory(app):
    """Reject non-system dependencies outside the bundle and absent architectures."""
    records = []
    for path in macho_paths(app):
        image = MachO(str(path))
        architectures = set()
        dependencies = set()
        deployment_targets = {}
        for header in image.headers:
            architectures.add(header.header.cputype)
            minima = [command.minos if lc.cmd == 50 else command.version
                      for lc, command, _ in header.commands if lc.cmd in (36, 50)]
            deployment_targets[CPU_ARCHITECTURES.get(header.header.cputype, 'unknown')] = validate_deployment(
                header.header.cputype, minima, path)
            for _, _, name in header.walkRelocatables():
                dependencies.add(name)
                if name.startswith(('/usr/lib/', '/System/Library/')):
                    continue
                if not name.startswith('@loader_path/'):
                    raise ValueError(f'Unresolved dependency in {path}: {name}')
                target = (path.parent / name[len('@loader_path/'):]).resolve()
                if not target.is_relative_to(app.resolve()) or not target.is_file():
                    raise ValueError(f'Dependency escapes bundle or is absent: {name}')
        if architectures != {0x1000007, 0x100000c}:
            raise ValueError(f'Expected Intel and Apple Silicon slices: {path}')
        records.append({'path': str(path.relative_to(app)), 'architectures': ['x86_64', 'arm64'],
                        'deployment_targets': deployment_targets,
                        'dependencies': sorted(dependencies)})
    return records

def archive_app(app, archive, full=True):
    """Preserve app symlinks and executable modes; verify each archived member."""
    paths = [app, *sorted(app.rglob('*'))]
    with zipfile.ZipFile(archive, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as stream:
        for path in paths:
            name = path.relative_to(app.parent).as_posix()
            if path.is_symlink():
                info = zipfile.ZipInfo(name)
                info.create_system = 3
                info.external_attr = (stat.S_IFLNK | 0o777) << 16
                stream.writestr(info, os.readlink(path).encode())
            else:
                stream.write(path, name + ('/' if path.is_dir() else ''))
    with zipfile.ZipFile(archive) as stream:
        # Full member reads already verify CRCs; do not decompress twice.
        if not full:
            if stream.testzip():
                raise ValueError('Archive CRC verification failed')
            return
        for path in paths:
            if path.is_dir() and not path.is_symlink():
                continue
            name = path.relative_to(app.parent).as_posix()
            expected = os.readlink(path).encode() if path.is_symlink() else path.read_bytes()
            if stream.read(name) != expected:
                raise ValueError(f'Archive content mismatch: {name}')
            if stat.S_IMODE(stream.getinfo(name).external_attr >> 16) != stat.S_IMODE(path.lstat().st_mode):
                raise ValueError(f'Archive permissions mismatch: {name}')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools',type=Path,default=Path('/workspace/tools/cab-cross'))
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--version',default=VERSION,choices=(VERSION,), help='Must match shared/version.h')
    parser.add_argument('--profile',choices=('prerelease','full'),default='prerelease')
    args=parser.parse_args();license_file("macos",args.version)
    tools=args.tools.resolve();output=args.output.resolve()
    output.mkdir(parents=True,exist_ok=False)
    app=output/(NAME+'.app');contents=app/'Contents';macos=contents/'MacOS';resources=contents/'Resources'
    macos.mkdir(parents=True);resources.mkdir()
    env=dict(os.environ,LD_LIBRARY_PATH=str(tools/'root/usr/lib/x86_64-linux-gnu'))
    sdk=tools/'sdk/MacOSX13.3.sdk'
    for name,source,objc,extra in (
        (NAME,ROOT/'frontends/macos/main.mm',True,[]),
        ('CabBackend',None,False,[]),
        ('CabPrivilege',ROOT/'frontends/macos/authorization.c',False,['-DCAB_PRIVILEGED_HELPER']),
        ('CabAuthorization.dylib',ROOT/'frontends/macos/authorization.c',False,['-dynamiclib','-framework','Security','-Wl,-install_name,@loader_path/CabAuthorization.dylib'])):
        def slice(arch):
            destination=output/(name+'-'+arch)
            if source is None:backend(tools,destination,arch,env)
            else:
                compiler=tools/('root/usr/bin/'+('clang++-19' if objc else 'clang-19'))
                flags=['--target='+arch+'-apple-macos'+TARGETS[arch],'-isysroot',str(sdk),
                       '--ld-path='+str(tools/'root/usr/bin/ld64.lld-19'),'-Oz','-flto=thin','-Wl,-dead_strip',
                       '-Wl,-platform_version,macos,'+TARGETS[arch]+',13.3']
                if objc:flags+=['-std=c++17','-fobjc-arc','-fblocks','-Wunguarded-availability','-framework','AppKit','-framework','Foundation']
                subprocess.run([str(compiler),*flags,*extra,str(source),'-o',str(destination)],env=env,check=True)
            if source is not None:
                subprocess.run([str(tools/'root/usr/bin/llvm-strip-19'),'-x',str(destination)],env=env,check=True)
            return destination
        with ThreadPoolExecutor(max_workers=2) as pool:parts=list(pool.map(slice,TARGETS))
        universal(macos/name,parts)
        for part in parts:part.unlink()
    shutil.copy2(ROOT/'assets/cab-disabled.icns',resources/'cab-disabled.icns')
    shutil.copy2(license_file('macos',args.version),resources/'LICENSES.txt')
    plist={'CFBundleExecutable':NAME,'CFBundleIdentifier':'io.github.metarch-ancilla.archetypecabdisabler',
           'CFBundleName':'Archetype Cab Disabler','CFBundleDisplayName':'Archetype Cab Disabler',
           'CFBundlePackageType':'APPL','CFBundleVersion':args.version,'CFBundleShortVersionString':args.version,
           'LSMinimumSystemVersion':TARGETS['x86_64'],'LSMinimumSystemVersionByArchitecture':TARGETS.copy(),
           'NSHighResolutionCapable':True,'NSRequiresAquaSystemAppearance':True,'CFBundleIconFile':'cab-disabled.icns'}
    (contents/'Info.plist').write_bytes(plistlib.dumps(plist))
    subprocess.run([str(tools/'codesign/apple-codesign-0.29.0-x86_64-unknown-linux-musl/rcodesign'),'sign',str(app)],check=True)
    records=inventory(app)
    if args.profile=='full':verify_app(app,list(macho_paths(app)))
    archive=output/f'{NAME.replace(" ","")}-{args.version}-macos-universal.zip';archive_app(app,archive,full=args.profile=='full')
    metadata=dict(version=args.version,frontend='native AppKit',backend='native C++17; no Python/Tk runtime',
        **source_state(),
        target='macOS x86_64 + arm64',deployment_targets=TARGETS.copy(),release_ready=False,native_execution='unverified',
        signing='ad hoc; not notarized',**artifact_record(archive),macho=records,
        optimization=dict(compiler='-Oz',lto='thin',symbols='local symbols stripped before signing',zip_level=9),
        validation_profile=args.profile,checks=dict(archive='CRC',architecture_dependencies_deployment='passed',ad_hoc_signing='completed'))
    (output/'build-info.json').write_text(json.dumps(metadata,indent=2)+'\n')
    print(json.dumps({k:v for k,v in metadata.items() if k!='macho'},indent=2))

if __name__=='__main__':main()
