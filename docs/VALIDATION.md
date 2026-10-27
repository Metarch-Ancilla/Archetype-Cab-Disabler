# Validation guide

Use this guide to verify current source and release candidates. It is not a record of completed tests. Record the source revision, application version, build profile, OS, CPU architecture, filesystem and observed outcome with each new report. Cross-compilation, Wine and archive checks do not establish native Windows or macOS acceptance.

Run checks relevant to the change. After backend changes, run the host regression, differential and workflow gates below. Reuse unchanged backend results for documentation or frontend-only work. Source-only requests must not build application packages; do not run syntax-only checks immediately before a real compilation. Never use GitHub Actions.

## Host backend and development checks

From the repository root, with the build tools and Python environment described in [cross-building](CROSS-BUILDING.md):

```sh
cmake -S native -B build/host -DCAB_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/host --parallel
ctest --test-dir build/host --output-on-failure
.venv/bin/python scripts/validate_cpp_backend.py --backend build/host/CabBackend --report /tmp/cab-differential.json
.venv/bin/python scripts/test_cpp_workflow.py --backend build/host/CabBackend --report /tmp/cab-workflow.json
```

Linux builds require OpenSSL development files. CTest exercises the production core through engine, format, regression, security and parser-mutation executables. The differential driver compares generated inputs with the test-only Python reference under `tests/reference`; Python is not application code or a distributed runtime. The workflow driver exercises the real backend pipe service, discovery, selection, review, backup and writes.

For frontend-only changes, check the affected target without producing application packages:

```sh
.venv/bin/python scripts/check_native_sources.py --target windows --report /tmp/cab-windows-sources.json
.venv/bin/python scripts/check_native_sources.py --target macos --report /tmp/cab-macos-sources.json
```

The macOS target checks both Intel 10.13 and Apple Silicon 11.0 API/deployment floors. Run only the affected platform. Build and license automation can be checked independently:

```sh
.venv/bin/python -m unittest tests.test_build_helpers tests.test_license_notices tests.test_windows_package tests.test_macos_bundle tests.test_macos_targets
```

For parser or filesystem hardening, also run a separate Debug CTest build with both `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS` set to `-fsanitize=address,undefined -fno-omit-frame-pointer`. The parser mutation target includes deterministic mutations and bounded malformed inputs; accepted inputs must still satisfy scalar-only patching. Before public builds, review the read-only upstream dependency check:

```sh
.venv/bin/python scripts/check_security_dependencies.py --report /tmp/cab-security-dependencies.json
```

A successful dependency query is not proof that every upstream report is inapplicable. See [security boundaries](SECURITY.md) and [licensing review](LICENSING_REVIEW.md).

## Preset correctness and corpus checks

For every supported structure, check Enable and Disable, an already-correct input, repeated application and an exact round trip. Exactly one unambiguous whole-cab scalar may change, including its own necessary serialized length. Compare bytes outside that scalar: names, versions, IDs, paths, amp settings, microphones and IR state must remain identical. Reject unknown, malformed, encrypted or ambiguous inputs without modifying them; a mic/IR switch is not a substitute for the whole-cab switch. See [format rules](ADAPTERS.md).

Public factory preset corpora are available through official [Neural DSP desktop plugin installers](https://neuraldsp.com/plugins). Keep locally installed or user-supplied files outside the repository and packages. For a controlled comparison, export custom presets from the running plugin with the cabinet enabled and disabled, changing only that control. These comparison exports are generated in the app; do not describe them as files supplied by the installer. Plugin saves can change unrelated fields, so isolate the cabinet evidence and verify that this editor preserves those other bytes. Structural recognition alone does not establish compatibility with every plugin or version; keep assessments scoped as in the [factory audit](FACTORY_PRESET_AUDIT.md).

Optional local corpus checks:

```sh
.venv/bin/python scripts/validate_cpp_backend.py --backend build/host/CabBackend --private-presets /path/to/local/presets --report /tmp/cab-private-differential.json
.venv/bin/python scripts/audit_preset_library.py --backend build/host/CabBackend --presets /path/to/local/presets --report /tmp/cab-local-audit.json
```

Use disposable copies for any write or plugin-loading acceptance check. Keep reports containing local paths private; publish only aggregate evidence, never preset contents.

## Native security diagnostics

Build scratch-only console probes locally; these commands do not build a GUI release:

```sh
.venv/bin/python scripts/build_security_diagnostics.py --platform windows --output build/security-windows
.venv/bin/python scripts/build_security_diagnostics.py --platform macos --output build/security-macos
```

Choose an output directory that does not already exist. Windows output is under `windows/`; macOS has separate `x86_64/` and `arm64/` directories. Copy the correct architecture's tools together. Distributed diagnostic binaries require the matching platform notices from `licenses/`. macOS binaries may need executable permissions restored; Gatekeeper policy still applies.

Each ordinary probe needs a fresh, non-existent local scratch directory without symlinks or junctions. It creates collision/replacement fixtures and leaves them for inspection. It never needs installed presets. Windows PowerShell:

```powershell
.\CabSecurityProbe.exe C:\CabSecurityScratch > security-results.txt
$LASTEXITCODE
```

Choose a writable real parent if the drive root is not writable. To exercise the restricted helper with an explicit UAC prompt, use a second new directory:

```powershell
.\CabSecurityProbe.exe C:\CabSecurityAuthorized --authorize > authorization-results.txt
$LASTEXITCODE
```

On macOS, use `/private/tmp`, because `/tmp` is a symlink. Run tools for the machine's architecture, keeping `CabAuthorizationProbe` and `CabPrivilege` from the same build together:

```sh
chmod +x CabSecurityProbe CabAuthorizationProbe CabPrivilege
./CabSecurityProbe /private/tmp/cab-security-new > security-results.txt
printf '%s\n' "$?"
./CabAuthorizationProbe --authorize "$PWD/CabPrivilege" /private/tmp > authorization-results.json
printf '%s\n' "$?"
```

The authorization scratch parent must already exist; that probe creates its own private child directory. Only explicit `--authorize` runs request administrator approval. Check exit code 0 and the ordinary probe's `passed` lines. Authorization reports must show selected-file identity, verified synthetic writes and rejection of an unapproved path. On macOS, inspect kernel-verified direct-child status and bridge reaping; `reaped_by_bridge: null` does not prove that another process reaped the helper. Inspect process lifetime separately in that case. Retain OS, architecture, filesystem, reports and exit codes without passwords or real presets.

Repeat consent denial and successful batches. Native application cancellation/close behavior must also be tested: failed or declined authorization means no edits; interruption after writes begin must accurately distinguish completed and unwritten files. Probes do not certify GUI behavior, network filesystems or all permission configurations. Wine may expose Unix mounts as reparse points; use a fresh path on its C: drive, and do not infer real UAC behavior from Wine.

## Native GUI acceptance

Use disposable preset libraries on real Windows and both supported Mac architectures, including the minimum OS targets where available. Modern macOS and Apple Silicon need their own execution and authorization checks; Intel or cloud results do not establish them. Treat untested combinations as unverified.

- Open the application at its initial installer-sized window. Confirm immediate display of the empty plugin table and inline `Indexing...`, then asynchronous population; Browse must repeat this state. Check keyboard use, resizing and title/header checkbox alignment in disabled, enabled and mixed states, inspecting actual screen captures.
- Confirm hidden default roots, Browse of a plugin root containing Presets, individual/folder/plugin selection and cascading mixed states. User presets start unchecked and can be selected in either edit mode. Folders precede files at every depth; displayed preset stems omit `.xml`. Reset All Settings.xml and both Cab Disabled/Cab Enabled directories must be excluded, including nested discovery and recursive copying.
- Exercise all four combinations: Enable/Disable with Direct Edit/Copy and Edit. Confirm Disable and Direct Edit defaults and immediate updates to the combined white description box, including the Direct-only permissions sentence. Back/Next must retain selections.
- Check Backup Yes/No, disabled read-only location with No, destination Browse and collision-suffixed ZIP names. Review must create no archive. Confirm separate Mode, Backup, Operation and Presets summary lines, a relative preset-only tree and the exact informational no-backup notice: “Backup not enabled. Proceed at your own risk.” The small native icon must be vertically centred; Apply must require no acknowledgement.
- With backup selected, independently reopen the completed ZIP and compare entry hashes with originals before allowing authorization or writes. Exercise existing, unwritable and interrupted backup destinations: failure/cancellation must leave all presets unchanged and never overwrite a backup. Archived preset/folder names must remain unchanged.
- Test writable Direct Edit and protected Direct Edit. Only the latter should request native consent when access requires it; GUI/backend/backup remain ordinary-user processes. Confirm denial writes nothing, successful writes preserve filename/location and file identity/ownership, and locks/read-only attributes fail clearly. Direct writes are non-atomic; verify flush/read-back behavior and truthful interruption results.
- In Copy and Edit, confirm no consent prompt, unchanged originals and embedded metadata, original filenames (including existing CD suffixes and extension case), relative subfolders and the correct Cab Enabled/Cab Disabled destination. Existing identical or conflicting copies must never be overwritten. Rediscovery must exclude both output directories.
- During work, confirm static `Working... X/Y` text and responsive window-close cancellation. Results must show Successful/Failed counts and relative trees with errors and already-enabled/disabled context. After completed Results, terminate/disconnect the backend: keep Results and enabled Finish, with no disconnect error replacing them. Finish must close locally. A disconnect before completion must still be reported.
- On Windows, inspect native vertical scrollbars, fixed-width header checkbox columns, live column resizing and plugin columns constrained to the viewport. Selection tables have no horizontal bar; Review wraps without one. Check resize/scroll/drag painting and native arrow styling without custom auto-hide expectations. On macOS, check Aqua/light appearance in the main window, dialogs and Licenses, white fields, grey disabled backup text, native scrollers and Review horizontal scrolling only when needed.
- Open Licenses from each page; verify complete version-matched notices, no wrapping, readable scrolling and usable resizing. No report export, New operation, separate Cancel button or graphical progress bar should appear.

## Package and release verification

Follow [cross-building](CROSS-BUILDING.md) and [releasing](RELEASING.md); use the default prerelease profile for prototypes and the full profile for stable releases, release candidates or an explicitly requested deep check. Do not rebuild solely for documentation changes.

Verify ZIP CRCs and exact package contents. Windows must contain only `Archetype Cab Disabler.exe` at ZIP root, with the backend/helper modes and complete license resource embedded. macOS must contain `Archetype Cab Disabler.app` with the expected universal binaries, architecture-specific deployment floors, system dependencies and valid ad hoc code/resource hashes. Ad hoc signing and SHA-256 checks are not Apple Developer ID trust or notarization. Check browser-download quarantine/Gatekeeper separately on a real Mac.

Exercise the actual packaged Windows wizard, using `scripts/smoke_windows_gui.py` where suitable, and inspect captures as well as control assertions. Wine is a development check; real Windows rendering/UAC and native Mac execution still require the acceptance checks above. Verify application/platform/license versions against `shared/version.h`, then verify uploaded sizes and SHA-256 digests against the local artifacts. Record limitations with the candidate's evidence rather than claiming unsupported native compatibility.
