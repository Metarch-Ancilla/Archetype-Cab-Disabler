# Distribution licensing

This is the maintained license review for the release build profiles: Windows
GCC/MinGW with static runtimes and macOS with system libraries. The application
is MIT licensed. Combining the notices in one file does not change any component's
license or apply every listed license to the application.

## Components and obligations

| Component | License and distribution treatment |
| --- | --- |
| Application | MIT. Preserve the root LICENSE, including the Metarch-Ancilla copyright, permission and disclaimer, in both platform notices. |
| Expat 2.9.0 | MIT. Retain COPYING and every distinct copyright line from the vendored C/header files. Source and configuration provenance are recorded in third_party/expat/PROVENANCE.md. |
| Expat's SipHash | CC0-1.0. Retain upstream attribution and the complete CC0 text in both notices. |
| Windows libgcc/libstdc++ | GPL-3.0-or-later WITH GCC-exception-3.1. Retain the complete pinned GCC copyright record, exception and GPL text. GCC C/C++ compilation, including single-partition LTO, qualifies for the runtime exception: the independent application can remain MIT without a GPL application-source offer. This does not cover copied compiler source or a separately modified runtime distribution. |
| Windows MinGW runtime/headers and winpthreads | File-specific ZPL, MIT, BSD/Lockless and public-domain notices. Retain the complete pinned MinGW package copyright record and referenced license texts. Do not imply upstream endorsement. |
| macOS libc++ headers | Apache-2.0 WITH LLVM-exception and upstream notices. Retain the matching libc++ 15.0.6 license. The LLVM exception covers embedded compiled header portions without Apache sections 4(a), 4(b), and 4(d). |
| OS libraries/frameworks | Windows system DLLs and macOS AppKit/Foundation/Security/libSystem/libobjc/system libc++ are OS supplied. Their binaries are not redistributed. |
| Build tools | Compilers, SDKs, Python, graphics utilities and Wine are not application dependencies or included in the downloads. Their build-use terms are separate from application distribution. |

The complete GCC/MinGW package records also describe unbundled tools, tests and
source files. Keeping those records intact preserves runtime provenance; it does
not mean those tools are shipped. The Microsoft-runtime build experiment is not
covered by this GCC/MinGW distribution review and requires its own assessment
before publication.

Factory presets used for format assessment are publicly available through
Neural DSP's official desktop plugin installers. They are not included in the
application or public test fixtures; use generated fixtures in the repository.
Public availability does not grant redistribution rights. The application icon
uses project-local geometric artwork; Neural DSP artwork is not a bundled asset.

## Notice files and access

- Windows: embed licenses/windows-LICENSES.txt verbatim as PE RCDATA resource 102.
  The ZIP contains only Archetype Cab Disabler.exe; no loose notice is needed.
- macOS: copy licenses/macos-LICENSES.txt to
  Archetype Cab Disabler.app/Contents/Resources/LICENSES.txt before ad hoc signing.
- Both: Licenses on each wizard page opens the complete notices in a native,
  read-only scrollable window. Keep matching platform TXT files on the release page.

MIT and applicable BSD/ZPL clauses permit complete notices in accompanying
material; separate files for each component are not required. Source notices
remain in their original repository locations.

## Maintenance and verification

The release package review found no missing notice or source-offer obligation for
these profiles. The embedded Windows notice and archived macOS notice matched
their source TXT files; published notices matched downloaded bytes, and package
sizes/digests matched build records. Recheck whenever the compiler profile,
runtime, SDK header version, third-party source, assets or package contents change.

1. Read the licenses and provenance of changed components. Keep upstream notices
   verbatim; a summary does not replace required legal text.
2. Update shared/version.h and regenerate both notices in the configured cloud:

   ```sh
   .venv/bin/python scripts/generate_licenses.py
   .venv/bin/python -m unittest tests.test_license_notices
   ```

3. Verify the Windows embedded resource and archived macOS notice against their
   respective TXT files. Confirm packages contain no compiler, SDK or unreviewed
   runtime files. Do not rely solely on a passing notice-generator test.
4. Publish matching platform notices and SHA-256 checksums with both packages;
   verify the remote assets. See [release packaging](RELEASING.md).

Review inputs include [the root license](../LICENSE),
[Expat's license](../third_party/expat/COPYING),
[Expat provenance](../third_party/expat/PROVENANCE.md),
[libc++ notices](../licenses/inputs/libcxx-15.0.6-LICENSE.txt), the vendored SipHash
header, and the pinned GCC/MinGW copyright records consumed by the generator.
