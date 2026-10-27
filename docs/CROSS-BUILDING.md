# Local cloud builds

Never use GitHub Actions. Reuse `/workspace/tools/cab-cross`; prepare missing pinned inputs only with `scripts/prepare_cross_tools.py`. Python is host build/test automation, never an application dependency. Build requirements are `requirements-build.txt`.

```sh
.venv/bin/python scripts/cross_build_windows.py --profile full --gcc-lto --output dist/windows-release
.venv/bin/python scripts/cross_build_macos.py --profile full --output dist/macos-release
```

The Windows builder uses MinGW directly and statically links the frontend, shared C++ backend and authorization bridge into one EXE. The EXE launches itself with `--backend` for the unprivileged pipe server, or with the existing restricted `--privileged-helper` mode for UAC. License text is an embedded resource viewed in-app. The ZIP contains only `Archetype Cab Disabler.exe` at its root, with no extraction at runtime and no loose DLLs, backend, license or build-info files.

The Mac builder uses cached Clang/LLD and SDK 13.3, compiles Intel 10.13 and arm64 11.0 slices, then combines the frontend/backend/authorization helper/bridge into universal binaries. It ad hoc signs the `.app`, inventories every Mach-O dependency and deployment target, and creates a zipped `.app`. No Python frameworks or shared stdlib are collected.

Native backend compilation is cached by source, shared protocol/text/version headers, parser, compiler-input manifest and target. Cache publication is atomic. Version metadata is shared from `shared/version.h`. CPU slices compile concurrently. macOS builds use `-Oz`, ThinLTO and local-symbol stripping before signing. Both ZIP profiles use deflate level 9. Fast prerelease mode signs, checks architecture/dependencies/deployment and performs one ZIP CRC pass. `--profile full` additionally performs deep Mac signature checks/archive comparison; Windows currently uses the same package gate. Reuse relevant test evidence for unchanged source. Do not run syntax-only passes immediately before real compilation, or rebuild just for docs/release links.

To build and test the host backend (no desktop packages):

```sh
cmake -S native -B build/host -DCAB_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/host --parallel
ctest --test-dir build/host --output-on-failure
```

CMake links the production backend and regression executables to the same `cab_core` and Expat libraries. Linux requires OpenSSL development files. `CAB_BUILD_TESTS` defaults to OFF for application builds. Native desktop CMake targets are under `frontends/`; platform-specific host packaging wrappers are in `scripts/build-windows.ps1` and `scripts/build-macos.sh`.

After backend changes run the native differential and workflow drivers:

```sh
.venv/bin/python scripts/validate_cpp_backend.py --backend build/host/CabBackend --report /tmp/cab-differential.json
.venv/bin/python scripts/test_cpp_workflow.py --backend build/host/CabBackend --report /tmp/cab-workflow.json
```

Factory preset samples are available through official Neural DSP desktop plugin installers; use local copies for checks. Custom exports stay outside source/packages. Wine can exercise the Windows package; it cannot prove real Windows UAC. macOS cross-package checks do not prove AppKit execution or notarization.

Commit final application source before building. Record clean source, version, sizes, SHA-256 and checks in external build-info JSON. Publish authorized releases via draft upload and verify remote size/digest once before publishing. Use prerelease status for test candidates and stable status when authorized. Build metadata must distinguish package checks from native acceptance.

The macOS app explicitly uses Aqua/light appearance; no dark-mode availability helper is needed. ZIP archive names omit spaces for GitHub portability; the .app and .exe names inside retain spaces.

## Windows GCC LTO release configuration

Pass `--gcc-lto` to `scripts/cross_build_windows.py`, or configure CMake Release with `-DCAB_GCC_LTO=ON`. Both use single-partition LTO (`-flto-partition=one`). Release builds use this option with the full profile; the script/CMake option remains off unless explicitly selected. The pinned GCC 14.2.0 crashes internally with ordinary partitioned LTO in this application. Single-partition LTO compiled successfully without disabling optimization passes. No additional runtime component or license is introduced.


Both platform license headers include the application version from `shared/version.h`. After changing that version, run `.venv/bin/python scripts/generate_licenses.py` before committing build inputs. Desktop builders reject mismatched headers so the embedded notices and release text files reference the same version as the application.
