# Native architecture

Archetype Cab Disabler is a native desktop wizard for Neural DSP desktop plugin presets. The same C++17 engine owns format handling and all preset writes on both platforms. Quad Cortex is outside its scope. The canonical repository is `Metarch-Ancilla/Archetype-Cab-Disabler`.

## Components and boundaries

| Component | Source | Responsibility |
|---|---|---|
| Shared backend | `native/backend/` | Discovery, strict parsing, selection, immutable review, backups, scalar editing, authorization orchestration, cancellation and CLI |
| Windows frontend | `frontends/windows/main.cpp` | Win32 wizard, native dialogs, embedded license viewer and restricted elevated-helper entry point |
| macOS frontend | `frontends/macos/main.mm` | AppKit wizard and dialogs for Intel and Apple Silicon |
| Native authorization | `frontends/windows/authorization.c`, `frontends/macos/authorization.c` | One-batch selected-file handle acquisition and native privilege boundary |
| Shared framing | `shared/text.hpp`, `shared/channel.hpp`, `frontends/shared/wire.hpp` | UTF-8/hex, bounded line framing, asynchronous queues and private pipe protocol v2 |
| Shared constants | `shared/version.h`, `shared/preset_copy.hpp` | Canonical application version and operation-dependent copy-folder names/descriptions |

Frontends send actions and render completed backend snapshots; they do not parse presets or write flags. The session's asynchronous worker owns its reviewed plan. Bounded queues operate before UI dispatch, command writers keep blocking pipe I/O off UI callbacks, and scan/model/plan budgets limit memory. Progress is coalesced; frontends update on STATUS/PROGRESS or a completed snapshot rather than each OPTION row. There are no runtime network listeners or external network requests, loaded plugins, Python runtime, telemetry or history database.

## Backend modules

| Module | Responsibility |
|---|---|
| `formats.cpp` | Strict bounded JUCE ValueTree and Expat XML validation; identify one unambiguous whole-cab scalar and preserve its representation |
| `platform.cpp` | Unicode paths, native identity/SHA-256, link-safe opens, direct writes with flush/read-back, atomic no-clobber publication and authorization handles |
| `discovery.cpp` | Generic preset-root discovery, deduplication and folder-first selection trees |
| `engine.cpp` | Reviewed signatures/hashes, operation plans, collisions, verified-backup ordering, write probes, selected-file authorization and results |
| `backup.cpp` | Exclusive stored ZIP/ZIP64, relative entries, metadata manifest, CRC/SHA-256 and complete reopened verification |
| `session.cpp` | Six-page wizard model, private pipe service, asynchronous work and cancellation |
| `main.cpp` | Independent CLI and backend-server entry points |
| `core.hpp` | Shared engine types, limits and interfaces |

## Review and write invariants

Format detection follows structure across plugin identities and versions, without plugin-name/version whitelists or schema hashes in the edit path. Exactly one unambiguous whole-cab scalar may change, including its own necessary serialized length. Every other byte is preserved: names, versions, IDs, paths and amp/mic/IR state. Unknown, encrypted or ambiguous inputs remain untouched; there is no whole-tree serialization, decryption or arbitrary byte searching.

Discovery builds selectable libraries and excludes generated `Cab Disabled` and `Cab Enabled` trees. Review fixes the selected sources, operation, identities and hashes before Apply. Copy and Edit preserves original filenames, relative folders and embedded metadata below the operation-dependent `Cab Disabled` or `Cab Enabled` library folder. Existing destinations are never overwritten.

Backup is optional. When selected, an exclusive ZIP preserves relative names and includes the metadata manifest. The complete archive is reopened and every entry/hash verified before any edit or authorization request. Failed or cancelled backup means zero edits. Only the default ZIP filename uses snake case; archived names remain unchanged.

Direct Edit writes through the existing opened file, preserving inode/ownership rather than replacing it. It checks reviewed identity/hash before writing, flushes/fsyncs, then verifies read-back. This write is non-atomic. Copies use private staging and atomic no-clobber publication.

`PreparedPath` retains lexical normalization; operation-scoped directory anchors resolve opens component by component without following symbolic links or Windows reparse points. Windows shares relative-opening helpers with its privileged C broker. `StagedFile` owns copy/backup staging and publication. POSIX enumeration uses directory-relative `fstatat` and retains device/inode metadata; Windows uses metadata-handle identity queries. Enumeration metadata assists discovery but never authorizes a later write. Fresh path and reviewed identity/hash checks remain necessary.

## Authorization boundary

At Apply, the ordinary-user engine verifies any selected backup and probes selected files without modifying them. Direct Edit asks for native authorization only when a selected file needs elevated write access. Copy and Edit never elevates. Authorization cancellation/failure means no edits.

The one-batch helper opens only the exact reviewed regular XML paths, rejecting linked path components, and returns descriptors/handles to the ordinary backend. The backend rechecks identity/hash and performs the scalar edit. The GUI, backend and backup stay unprivileged; no shell, ACL/owner changes, persistent service or privileged backup writes are involved.

On Windows, the main EXE also hosts the restricted `--privileged-helper` entry point. The native bridge launches it through explicit `runas`/UAC; the main manifest remains `asInvoker`. A private local-only pipe uses a current-user/admin/System DACL and validates both peer PIDs before selected-file handles are duplicated into the original backend.

On macOS, a small native helper and authorization bridge use a versioned PID handshake, deadlines, cancellation and a bounded specific-child reaper. The one-batch launch API is deprecated; replacement is deferred. See [security](SECURITY.md) for the threat boundary and diagnostic procedures, and [validation](VALIDATION.md) for native acceptance procedures and remaining limits.

## Platform packaging and support

Windows links the shared backend and authorization bridge into `Archetype Cab Disabler.exe`. An ordinary-user child runs the same EXE with `--backend`, retaining the private pipe boundary. The ZIP contains only that EXE: notices are embedded as RCDATA 102, and there is no runtime extraction, separate backend/helper EXE or authorization DLL. C++ runtime and Expat are statically linked. The interface uses native Win32 scrollbar ownership; Windows Review wraps without horizontal scrolling.

macOS packages `Archetype Cab Disabler.app` with four universal Mach-O files: AppKit frontend, backend, authorization helper and bridge library. Intel targets macOS 10.13; Apple Silicon targets 11.0. Native file APIs avoid a newer `std::filesystem` runtime requirement. Packages use system libc++, system crypto and statically linked Expat. AppKit stays in standard Aqua/light appearance.

Windows releases are unsigned. Mac packages retain ad hoc signing and are not notarized; publisher signing/notarization is declined and is not a release prerequisite. Browser-download quarantine may trigger Gatekeeper, and hash verification does not establish Apple trust. Cross builds and Wine checks do not establish execution or compatibility on actual Windows/macOS architectures; see [validation](VALIDATION.md) and [cross-building](CROSS-BUILDING.md).

Private user presets and plugin-generated comparison exports are local verification inputs, never source/package contents. Factory presets are publicly available from official Neural DSP desktop plugin installers; comparison exports generated in the plugin are not described as distributed installer files. Generated fixtures and native tests live under `native/tests/`; Python scripts are development tooling only.
