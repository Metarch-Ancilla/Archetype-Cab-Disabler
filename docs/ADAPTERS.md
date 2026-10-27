# Shared preset format rules

The shared C++ backend in `native/backend/formats.cpp` detects formats from file
bytes, not the `.xml` extension. It recognizes structural layouts across plugin
identities and versions; product names, version whitelists and sample-schema
hashes do not control editing. Desktop Neural DSP presets are in scope; Quad
Cortex is not.

## Readers and scalar edits

- Binary JUCE ValueTree: bounded typed tree parsing retains the original byte span of each scalar, including its encoded length.
- Text XML: UTF-8 and UTF-16, supported BOMs, attributes and simple leaf-element properties. Original encoding, quotes, whitespace, comments and unrelated text are preserved. DTD/entity declarations are forbidden; a cab scalar requiring entity expansion or whitespace normalization is rejected.
- Parsing is limited to 4 MiB, 64 tree levels and a bounded node/property budget. Malformed, unknown, encrypted or compressed inputs are left unchanged.

Exactly one unambiguous whole-cab scalar is required. The backend splices that
scalar into the original bytes, reparses the result, verifies the requested state
and metadata, and compares every byte before and after the scalar. It does not
serialize the whole tree, decrypt data or search arbitrary byte substrings.
Already-requested states return the original bytes.

Boolean, integer, int64 and double values use true/1 for enabled and false/0 for
disabled. Strings support `true`/`false`, `True`/`False`, `TRUE`/`FALSE`, `0`/`1`
and decimal forms such as `0.000`/`1.000`. The scalar's type and supported string
style are retained; its necessary binary serialized length can change. Unsupported
values or types are rejected. This polarity also applies to the exact legacy
`cabsim_bypass` identity, whose semantics remain assessed rather than confirmed
by a controlled whole-cab comparison.

## Recognition rules

| Layout | Required structure |
| --- | --- |
| Named `PARAM` | One unique string `id` equal to `cabActive`, `cabSectionActive`, `cabActiveID` or `cabsim_bypass`, with one `value`. A recognized node contains exactly the two attributes, or exactly two simple child elements named `id` and `value`. Mixed or extra content is rejected. |
| Whole-cab property | `cabActive` or `cabSectionActive` on the root or a `parameters`, `appModel` or `cabParameters` node. |
| Hierarchical section | `sectionActive` property on `cabParameters`; arbitrary amp or microphone `sectionActive` properties are excluded. |
| Legacy attribute | Exact `cabsim_bypass` property on `Parameter`. There is no generic bypass-name rule. |
| Numeric `PARAM` | `PARAM[id=400].value` only when its parent contains the shared two-slot parameter groups and explicit IR-path container described in the [factory audit](FACTORY_PRESET_AUDIT.md#numeric-id-cab-section). The selected node has exactly the `id` and `value` properties and no child elements. Numeric `400` alone is insufficient. |

`cabActiveID` is recognized only as an exact `PARAM` identity, never as an
arbitrary property or suffix match. Duplicate sibling `PARAM` identities,
multiple eligible whole-cab switches and ambiguous property representations
reject the input. Additional unrelated settings and unseen metadata are allowed
and preserved. Microphone-slot and IR switches are never substitutes for the
whole-cab scalar. Multiple independent cab sections remain unsupported.

## Evidence and confidence

The factory presets used for structural coverage are publicly available through
official Neural DSP desktop plugin installers. Custom saved on/off exports used
for comparison are separate validation evidence; they are not described as
installer-distributed files. Neither those exports nor factory preset contents
are bundled in this repository.

| Evidence | What it establishes | Limits |
| --- | --- | --- |
| Whole-cab on/off export comparisons for named `cabActive`, hierarchical `cabParameters.sectionActive` and guarded numeric `400` layouts | Mapping/polarity evidence for the compared layouts, plus scalar-only editing checks on disposable copies | A comparison for a layout does not test every plugin/version listed under it. The numeric comparison is Tom Morello; Mesa plugin behavior is not independently verified. |
| Factory catalog: 3,677 XML files across 19 plugin folders | Both-direction edit integrity for the observed structures: 3,664 binary ValueTrees and 13 text XML files | Catalog coverage does not establish plugin-load or audible correctness. NTS polarity remains assessed. See the [audit](FACTORY_PRESET_AUDIT.md). |
| Generated binary and text XML fixtures | Scalar types/styles, unseen identities/versions, encoding, Unicode, formatting, round trips, ambiguity rejection and malformed inputs | Generated cases verify shared rules rather than individual plugin behavior. |

A controlled comparison loads a preset in the plugin, saves an export, toggles
only the whole-cab control, saves another export and compares the pair. Plugin
saving can also change metadata, IR paths or other serialized values. Those
export differences are evidence to assess; the tool changes only the identified
whole-cab scalar. Comparison evidence and plugin associations match the
layout-level Tested column in the [README](../README.md#supported-plugin-formats).

Legacy Nolly uses exact `PARAM[id=cabActiveID].value` as a double. Its
comparison pairs did not change that flag, so whole-cab behavior remains
unverified. Support uses the provisional assumption that 1 is enabled and 0 is
disabled, based on the parameter name and its distinction from microphone-slot
switches. Scalar-only edit integrity is verified; neither filenames nor mic
states establish whole-cab polarity. Enabling the whole cab leaves disabled mic
slots disabled.

## Local verification

`scripts/audit_preset_library.py` uses the native backend and an independent
strict test-only Python tree reader to verify both directions on temporary
copies. It checks state, type, unchanged unrelated parsed values, exact bytes
outside the scalar, idempotence and reverse round trips. Generated format tests
live in `tests/test_formats.py`, `tests/test_valuetree.py` and
`native/tests/formats.cpp`.

Copy and Direct Edit use the same validated patch. Copies retain embedded
metadata, original filenames and relative folders under Cab Enabled or Cab
Disabled. File-write and backup behavior is documented in the
[architecture](ARCHITECTURE.md) and [feature reference](FEATURES.md). Format
recognition alone does not establish plugin-browser behavior or compatibility
with every released plugin version.
