---
name: cloud-desktop-build
description: Build the shared C++17 backend and native Win32/AppKit frontends locally in cloud.
---

Read `docs/ARCHITECTURE.md` and `docs/CROSS-BUILDING.md`. Use the current checkout and cached `/workspace/tools/cab-cross`. Never use GitHub Actions. Source-only requests mean no application compilation/packaging; syntax-only checks are allowed. User instructions take precedence.

Keep parsing, selection, backup and edits in `native/backend`; native frontends share private pipe protocol v2. No Python runtime, Tk/Tcl, PyObjC or duplicate edit engines. The Python oracle under `tests/reference` is test-only. Keep private presets outside source and packages.

For authorized builds use `scripts/cross_build_windows.py --output <new-directory>` and/or `scripts/cross_build_macos.py --output <new-directory>`. Default prerelease mode prioritizes speed. Reuse native cache and unchanged validation. Do not repeat setup, suite runs, syntax checks before compilation or package gates already performed by builders. After backend changes run the native differential/workflow checks; run target tests only for relevant platform behavior. Full Mac profile adds deep signatures/archive comparison.

Windows uses MinGW with statically linked C++/Expat and a single GUI/backend/elevated-helper EXE with embedded notices and no runtime extraction. Mac uses Clang/LLD, system libc++, statically linked Expat and four universal binaries; target Intel 10.13/arm64 11.0. Fresh apps are ad hoc signed and architecture/dependency/deployment checked; ZIP CRC and SHA-256 are recorded. Notices remain required. No build metadata or test artifacts inside runtime archives.

For prerelease uploads commit application inputs first, record clean source/checksums, upload a draft, verify remote size/digest once, then publish. Existing user authorization permits releases without another token. Never claim actual macOS execution, notarization or real UAC from cross-build/Wine checks. Keep release_ready false pending native acceptance. Do not rebuild unchanged apps for docs or download-link updates.
