# Security architecture and maintenance

This document describes the current source's security boundaries and accepted
limits. Build, regression and native diagnostic procedures are maintained in
[VALIDATION.md](VALIDATION.md). Cross-compilation and Wine checks do not establish
native operating-system acceptance or a security certification.

## Threat model and privilege boundary

Preset contents, filenames and filesystem state are untrusted. The application
must reject unsupported or ambiguous inputs, detect changes since review, avoid
overwriting existing copies/backups and bound work from malformed inputs or slow
local IPC peers. Concurrent filesystem replacement is a local threat; arbitrary
malicious code running as the same account, replacement of the application/helper,
and changes made after verification are outside the guarantees below.

The native GUI and shared C++ backend run as the ordinary user. Parsing, backup
creation, edit planning and all preset writes stay in `native/backend/`. There is
no runtime networking, telemetry, plugin loading, shell execution, installed
privileged service or persistent authorization daemon. Private inherited IPC
connects each frontend to its backend; it is not a public service endpoint.

Only Direct Edit can request administrator approval, and only when a selected
file requires elevated write access. A requested backup must finish and verify
before the prompt. A one-batch native broker opens exact reviewed regular `.xml`
paths and transfers writable handles/descriptors to the ordinary-user backend.
It does not parse presets, write their contents, change ownership/ACLs or create
backups. No folder-root restriction is imposed on explicitly selected files.
Copy and Edit never requests elevation. Initial authorization failure or decline
means no edits; cancellation after writing begins must preserve the distinction
between completed and unwritten files.

## Exact-byte editing and parser limits

The shared format detector supports desktop Neural DSP presets without plugin
name/version whitelists. Exactly one unambiguous whole-cab scalar may change,
including its own necessary serialized length. All other bytes, including names,
IDs, paths, amp state and microphone/IR settings, are preserved. There is no
whole-tree serialization, decryption or arbitrary byte search. Unknown,
encrypted, ambiguous or unsafe representations are rejected without modification.

The file limit is 4 MiB; XML and ValueTree parsing enforce depth 64 and a 20,000
event/node budget. Plan data is bounded at 64 MiB. Keep those limits, literal
byte-span validation and exact-scalar regression checks in place when adding
format support. Successful structural parsing alone does not establish support
for every plugin.

Expat **2.9.0** is vendored with namespace expansion, external DTDs, general
entities and diagnostic input-context buffering disabled. Every DOCTYPE is
rejected. Checked operating-system entropy supplies a 128-bit parser hash salt;
64-bit Expat byte-offset APIs locate literal spans in the original input.
See [dependency provenance/configuration](../third_party/expat/PROVENANCE.md).

Check upstream releases and advisories before public builds and at least monthly
during active maintenance. Use `scripts/check_security_dependencies.py`; record
version, digest, configuration and relevance of disclosed fixes in the dependency
provenance. Maintain bounded mutation/sanitizer coverage of the production parser
configuration, including malformed encodings, dense attributes, deep structures,
truncated ValueTrees, extreme lengths and callback-stop paths. Turn reproducible
failures into small public synthetic regressions; never commit private presets.

Expat's [upstream report tracker](https://github.com/libexpat/libexpat/issues/1160)
includes non-public reports whose applicability cannot be determined from public
information. Their status remains unknown. A current dependency version is not
proof that these reports are fixed or unreachable. If a concrete reachable flaw
has no patch, isolate the affected representation or apply a narrow mitigation
with a regression test. Any future hard parser-time limit should use a killable
ordinary-user parsing worker, not interrupt the editing backend during a write.

## Filesystem writes and backups

Preview records source identity and SHA-256. Apply checks identity, reviewed bytes
and path identity again at the write boundary. Operations use scoped directory
anchors: component-relative no-follow opens on POSIX, and component-relative
`NtCreateFile` plus handle type/reparse checks on Windows. Unexpected access or
filesystem errors fail the operation; they are not treated as missing paths.

Direct Edit writes into the existing opened file, preserving inode/ownership and
requiring file write access rather than directory replacement permission. It does
not rename or replace the original. The backend flushes/fsyncs and verifies
read-back bytes. **Direct writes are non-atomic:** interruption or write failure
can leave a partially modified file. The failure report must say so. Backup is
optional by explicit user choice; there is no automatic rollback guarantee.

When selected, the complete ZIP remains staged while it is reopened and every
entry's hash/CRC is verified. Only then is it published, without replacing an
existing ZIP, before the first preset edit. Backup failure or cancellation blocks
all edits. Backups contain original preset contents and a manifest with source
paths and metadata; treat them as private user data.

Copies preserve relative folders and original filenames under the operation's
`Cab Enabled` or `Cab Disabled` folder. Copy/backup publication uses private POSIX
staging or a Windows staging handle that denies write/delete sharing. Windows
publishes through handle-based no-clobber rename; there is no full-path fallback.
Cleanup never deletes the requested user-visible destination. POSIX skips staging
directory cleanup under group/world-writable parents; leftover temporary data is
preferable to deleting a substituted name.

These protections do not make arbitrary concurrent writes universally safe.
POSIX staging cleanup still uses names, and private directories do not protect
against arbitrary same-account code. Inherited ACLs, supported filesystem/share
semantics, hard links and post-verification mutation require care. Unsupported
filesystem operations must fail clearly rather than use weaker path fallbacks.

## IPC bounds and cancellation

Keep aggregate budgets as well as individual record limits. Backend command
queues are bounded by bytes/count; progress/status updates coalesce. Frontends
bound received bytes before posting a UI wakeup, rather than posting unlimited
payloads to the native event queue. Command writes run off the GUI thread, with
bounded queues, blocked-write deadlines and shutdown wakeups. Urgent cancel/quit
must remain usable when ordinary command capacity is exhausted. Protocol overflow
or malformed data must fail clearly and trigger cancellation/cleanup.

Model/snapshot, scan-path and search-root budgets complement the 64 MiB line
limit. The authoritative limits and behavior live in
[`shared/channel.hpp`](../shared/channel.hpp),
[`shared/text.hpp`](../shared/text.hpp),
[`frontends/shared/wire.hpp`](../frontends/shared/wire.hpp) and
[`native/backend/session.cpp`](../native/backend/session.cpp). Exercise valid
maximum-size snapshots as well as slow/paused peers, overflow and close during
transfer when changing these boundaries.

## Native authorization adapters

Windows uses explicit `runas`/UAC only when needed. Restricted helper mode is in
the main EXE, selected by `--privileged-helper` before GUI/backend startup; the
normal manifest remains `asInvoker`. The broker requires elevation, exact
arguments and an exact selected-file allowlist. A local-only private named pipe
has a current-user/Administrators/System DACL, and both peer PIDs are checked.
Handles are duplicated into the original backend process. Keep the shared
anchored-opening policy in the broker; do not add a service, shell command or
elevated GUI/backend. See
[`frontends/windows/authorization.c`](../frontends/windows/authorization.c).

macOS retains the deprecated `AuthorizationExecuteWithPrivileges` API for one
batch, preserving Intel macOS 10.13 and Apple Silicon macOS 11 compatibility.
The versioned startup handshake precedes the allowlist. Nonblocking I/O uses
monotonic absolute transfer deadlines, cancellation callbacks and helper idle
timeouts. The current transfer deadline is 30 seconds per transfer, not a limit
on the password dialog or entire batch. Descriptor receipt rejects partial,
duplicate or truncated replies, closes received FDs on error and checks
close-on-exec setup.

A bounded reaper reserves capacity before launch and retains only helpers whose
direct-child relationship is kernel-verified using specific-PID `waitpid`.
Deferred reaping continues until exit or `ECHILD`. Reported PID/PPID values are
lifecycle metadata, not authentication. Never use `waitpid(-1)` or signal a merely
reported PID. Apple's launch mechanism does not guarantee the backend owns every
child; pre-handshake failures and non-child helpers need native lifecycle
observation. See
[`frontends/macos/authorization.c`](../frontends/macos/authorization.c) and
[`frontends/shared/privilege_io.h`](../frontends/shared/privilege_io.h).

## Accepted limits and future decisions

Windows distribution is unsigned; macOS packaging is ad hoc signed and
unnotarized. Publisher signing/notarization is declined. Checksums check integrity,
not independent publisher identity or Apple trust; browser quarantine can still
trigger Gatekeeper. Do not add signing credentials or paid-service requirements
to build gates.

Replacing the legacy macOS elevation API is deferred. Before changing it, decide
whether installed helper registration and update/removal lifecycle are acceptable
while retaining the 10.13 floor. `SMAppService` starts at macOS 13; `SMJobBless`
supports older systems but is also deprecated on modern macOS. A replacement
must retain selected-file descriptor-only operations, ordinary-user editing and
OS-backed client authentication with explicit per-operation authorization. Do not
silently raise the minimum OS, elevate the GUI or treat a PID/Team ID/XPC endpoint
alone as authentication.

Current macOS/Apple Silicon native acceptance remains unverified because hardware
was unavailable. GUI consent/decline, cancellation, slow peers, early helper
failure and filesystem/ACL/share behavior need native checks; cross-builds and
scratch diagnostics cannot certify them. Diagnostic tools are built locally and
are not included in the stable 1.0.0 release. Follow
[VALIDATION.md](VALIDATION.md) for current procedures and preserve OS,
architecture, filesystem and exit codes with any new evidence.
