"""Build scratch-only native security probes, never the GUI or a release package."""
import argparse
import os
from pathlib import Path
import subprocess
from build_common import ROOT, EXPAT_SOURCES
from cross.macos_targets import TARGETS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=('windows', 'macos'), required=True)
    parser.add_argument('--tools', type=Path, default=Path('/workspace/tools/cab-cross'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    tools=args.tools.resolve();env=dict(os.environ,LD_LIBRARY_PATH=str(tools/'root/usr/lib/x86_64-linux-gnu'))
    targets=['windows'] if args.platform=='windows' else list(TARGETS)
    for target in targets:
        out=args.output/target;out.mkdir()
        if target=='windows':
            cc=str(tools/'root/usr/bin/x86_64-w64-mingw32-gcc-posix')
            cxx=str(tools/'root/usr/bin/x86_64-w64-mingw32-g++-posix')
            flags=['-D_WIN32_WINNT=0x0A00','-Os']
            link=['-static','-static-libgcc','-static-libstdc++','-s','-lbcrypt','-lshell32','-lole32','-luuid','-ladvapi32']
        else:
            cc=str(tools/'root/usr/bin/clang-19');cxx=str(tools/'root/usr/bin/clang++-19')
            flags=['--target='+target+'-apple-macos'+TARGETS[target],'-isysroot',str(tools/'sdk/MacOSX13.3.sdk'),'-Os']
            link=['--ld-path='+str(tools/'root/usr/bin/ld64.lld-19'),'-Wl,-dead_strip','-Wl,-platform_version,macos,'+TARGETS[target]+',13.3']
        objects=[]
        for name in EXPAT_SOURCES+(('random_rand_s',) if target=='windows' else ()):
            obj=out/(name+'.o');objects.append(str(obj))
            subprocess.run([cc,*flags,'-DHAVE_EXPAT_CONFIG_H','-DXML_STATIC','-I',str(ROOT/'third_party/expat'),'-c',str(ROOT/'third_party/expat'/(name+'.c')),'-o',str(obj)],env=env,check=True)
        if target=='windows':
            for name,defines in [('auth-helper',['-DCAB_PRIVILEGED_HELPER']),('auth-bridge',['-DCAB_WINDOWS_STATIC_AUTH'])]:
                obj=out/(name+'.o');objects.append(str(obj))
                subprocess.run([cc,*flags,*defines,'-c',str(ROOT/'frontends/windows/authorization.c'),'-o',str(obj)],env=env,check=True)
            flags+=['-DCAB_WINDOWS_STATIC_AUTH','-DCAB_WINDOWS_SINGLE_EXE','-DCAB_SECURITY_PRIVILEGED_TEST']
        sources=[ROOT/'native/tests/security.cpp',*[ROOT/'native/backend'/(name+'.cpp') for name in ('platform','formats','backup','engine','discovery')]]
        subprocess.run([cxx,*flags,'-std=c++17','-DXML_STATIC','-I',str(ROOT/'third_party/expat'),*map(str,sources),*objects,'-o',str(out/('CabSecurityProbe.exe' if target=='windows' else 'CabSecurityProbe')),*link],env=env,check=True)
        if target!='windows':
            subprocess.run([cc,*flags,str(ROOT/'native/tests/macos_authorization.c'),str(ROOT/'frontends/macos/authorization.c'),'-framework','Security','-o',str(out/'CabAuthorizationProbe'),*link],env=env,check=True)
            subprocess.run([cc,*flags,'-DCAB_PRIVILEGED_HELPER',str(ROOT/'frontends/macos/authorization.c'),'-o',str(out/'CabPrivilege'),*link],env=env,check=True)
        for obj in objects:Path(obj).unlink()
    print('Built diagnostic tools only. See docs/VALIDATION.md for native execution.')


if __name__=='__main__':main()
