# Feature reference

This describes current source behavior; see the [README](../README.md) for published build availability.

## Purpose and platforms

Offline batch enable/disable of the whole cabinet section in desktop Neural DSP amp-sim presets. No plugin loading, runtime networking, telemetry, activation or Quad Cortex support. Native Win32 frontend, universal AppKit frontend and shared C++17 backend; no Python runtime is distributed. Windows x64; macOS deployment targets Intel 10.13 and Apple Silicon 11.0. Native acceptance remains separate from cross-build validation.

## Discovery and selection

The installer-sized window opens immediately with an empty plugin table and inline Indexing notification. Default preset roots are hidden. Browse accepts a plugin’s top-level folder with nested Presets, repeats asynchronous indexing and adds discovered folders. Plugin names derive from preset folder names. Tables expose plugin name, location and selection checkboxes with a mixed-state global header checkbox.

The preset tree supports individual files, entire subfolders and whole plugins. Parent checkboxes cascade and reflect mixed selections. User presets begin unchecked but are selectable for either edit mode. Folders precede loose files, including Default.xml, at every level. Double-clicking expandable preset rows expands/collapses them. Plugin groups have separators. Displayed preset names omit .xml without renaming files.

Reset All Settings.xml is excluded case-insensitively at all depths, including Browse. Generated Cab Disabled and Cab Enabled directories are excluded. Paths and file identities are deduplicated; symbolic links/reparse points are rejected by safety checks.

## Wizard

Six pages: Select Plugins, Select Presets, Backup, Edit Mode, Review, Results. Next/Back navigation; the window close control handles cancellation. No tabs, graphical progress bar, export report or separate Cancel button. Static Working... X/Y reports successful edits while work runs. Finish closes the tool.

Backup uses Yes/No radio buttons. The read-only location defaults to Desktop and is disabled when No is selected; Browse chooses a future destination. Filenames use neural_dsp_presets_YYYY_MM_DD.zip with `_2`, `_3`, etc. for collisions. Snake case applies only to the ZIP filename; archived preset/folder names remain unchanged. No archive is created during review.

Edit Mode has exclusive Enable/Disable and Direct Edit/Copy and Edit checkbox pairs. One white box beneath the edit-mode buttons shows the cab action, then the edit description, then the permissions note for Direct Edit; it updates for either selection. A blank line separates the buttons from the box. Disable and Direct Edit appear on the left and are selected by default. Review shows Mode, Backup (No or the backup path), Operation (Enable/Disable) and selected count as unboxed text above a bordered list containing only the indented relative preset tree. When No backup is selected, an unboxed “Backup not enabled. Proceed at your own risk.” notice with a small native warning icon vertically centred beside the text appears above navigation; it requires no acknowledgement. Results show Successful/Failed counts and a relative file tree with errors and already-enabled/disabled context. Once Results is complete, backend disconnection does not replace it or disable Finish on either platform; Finish closes locally.

Windows uses the native Win32 list, tree and edit control scrollbars, with no custom auto-hide, hover timers or scrollbar style interception. Both selection headers have fixed checkbox widths and live column resizing. Plugin columns are constrained to the viewport, preventing horizontal overflow; its vertical scrollbar starts below the separate header. Presets retain the native no-horizontal-scroll flag. Windows Review wraps and has no horizontal scrollbar. macOS retains its native autohiding scrollers. End-arrow styling follows the operating system’s native theme. Mac Results wraps to its viewport. Licenses is a blue underlined link at every page’s top right; it opens complete notices in a read-only scrollable native window without wrapping.

macOS uses the standard Aqua/light appearance on all supported systems, including macOS 10.13. Boxed fields remain white; disabled path text remains grey.

## Editing

All four combinations of cabinet state and edit mode are supported. Only one unambiguous whole-cab scalar changes, including its own necessary encoded length; all other bytes are preserved. No whole-tree serialization or per-product/version whitelist. Unknown, malformed, encrypted or competing candidates remain unchanged with a reason. Mic and IR switches are never substituted for the cabinet switch. See [format rules](ADAPTERS.md) and [factory evidence](FACTORY_PRESET_AUDIT.md) for supported structures and provisional assessments.

Direct Edit writes into the existing file, preserving inode/ownership, filename and location. It checks reviewed identity and bytes before writing, then flushes and verifies read-back. Writes are non-atomic; interruption can damage files. Copy and Edit writes beneath each library’s Cab Disabled or Cab Enabled directory, according to the selected operation, preserves relative folders and original filenames with atomic no-clobber publication. Embedded preset names/IDs remain unchanged. Already-correct files are not unnecessarily rewritten.

## Backup and elevation

Optional ZIP/ZIP64 backup contains original bytes for planned writes, preserving a common-relative tree directly under presets/. Multiple Windows drives/shares have distinct namespaces when required. backup-manifest.json records archive entries, source paths, SHA-256, timestamp and mode. The completed ZIP is reopened and CRC/content/hash verified before any edit or authorization. Failure or cancellation prevents edits; existing backups are never replaced. Restoration is manual; no in-app restore exists.

Only selected files requiring protected write access trigger authorization at Apply, after backup verification. GUI, backend and backups remain unprivileged. Copy mode never prompts. A restricted one-batch helper opens only reviewed regular XML paths and passes native handles to the normal backend; no ACL/ownership changes, persistent service, shell or elevated edit engine. Declined authorization prevents editing. File locks and read-only attributes can still prevent writes.

## Distribution

Windows ZIP contains only Archetype Cab Disabler.exe at its root. Shared backend and authorization code are statically linked; the same executable runs separate --backend and restricted --privileged-helper modes. No runtime extraction or loose DLL/backend/license files. License text is embedded as a resource.

macOS ZIP contains a universal .app with native frontend, backend, authorization helper/bridge and consolidated notices. It is ad hoc signed without Apple Developer ID signing or notarization. Browser downloads can acquire quarantine metadata and trigger Gatekeeper independently of whether a USB copy opened; ad hoc signing does not confer a trusted developer identity. Icons retain all platform sizes with lossless PNG compression and consistent canvas margins. Platform notices retain third-party credits and project attribution to Metarch-Ancilla.

The product name is Archetype Cab Disabler. Runtime filenames are `Archetype Cab Disabler.exe` and `Archetype Cab Disabler.app`; the repository URL and existing release assets remain stable. The header checkbox uses the same client origin/column width during indexing and after population.

## Security boundaries

File opens use fresh directory anchors; copies/backups stage and verify before
no-clobber publication. Backup failure blocks editing. Direct edits remain
non-atomic and keep identity/hash, flush and read-back checks. IPC queues/model
sizes are bounded and GUI command writes run off-thread. The existing macOS
one-batch authorization bridge adds deadlines, cancellation and specific-child
reaping. Publisher signing/notarization is declined; replacement of the legacy
macOS elevation API is deferred. See [security boundaries](SECURITY.md),
[verification instructions](VALIDATION.md) and [licensing review](LICENSING_REVIEW.md).
