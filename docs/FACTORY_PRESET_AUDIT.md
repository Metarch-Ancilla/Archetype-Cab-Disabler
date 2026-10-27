# Factory preset coverage and schema evidence

The audited factory catalog contains 3,677 XML files across 19 plugin folders,
plus 176 WAV files. These factory presets are publicly available through official
Neural DSP desktop plugin installers. WAV files are IR/audio data and are not
edited. Factory preset contents and custom validation exports are not bundled
in this repository.

Every XML file was inspected: 3,664 are binary JUCE ValueTrees despite their
extension, and 13 are text XML. Product names below describe observed coverage;
they do not appear in the shared format detection/editing rules.

| Plugin folder | XML files | Whole-cab layout |
| --- | ---: | --- |
| Archetype Abasi | 134 | Named `PARAM`: `cabActive` |
| Archetype Cory Wong X | 233 | `cabParameters.sectionActive` |
| Archetype Gojira X | 322 | `cabParameters.sectionActive` |
| Archetype John Mayer X | 398 | `cabParameters.sectionActive` |
| Archetype Misha Mansoor X | 423 | `cabParameters.sectionActive` |
| Archetype Nolly X | 239 | `cabParameters.sectionActive` |
| Archetype Petrucci X | 187 | `cabParameters.sectionActive` |
| Archetype Plini X | 189 | `cabParameters.sectionActive` |
| Archetype Tim Henson X | 189 | `cabParameters.sectionActive` |
| Archetype Tom Morello | 139 | Numeric `PARAM`: `400` |
| Darkglass Ultimate | 141 | `cabParameters.sectionActive` |
| Fortin Cali Suite | 211 | Named `PARAM`: `cabActive` |
| Fortin NTS Suite | 32 | `cabsim_bypass` |
| Fortin Nameless Suite X | 177 | `cabParameters.sectionActive` |
| Mesa Boogie Mark IIC+ Suite | 161 | Numeric `PARAM`: `400` |
| Morgan Amps Suite | 108 | `cabParameters.sectionActive` |
| OMEGA Ampworks Granophyre | 46 | Named `PARAM`: `cabActive` |
| Soldano SLO-100 X | 220 | `cabParameters.sectionActive` |
| Tone King Imperial MKII X | 128 | `cabParameters.sectionActive` |

## Catalog scope and discovery

The counts include four root-level `Reset All Settings.xml` files: Abasi,
Fortin Cali, Tom Morello and Mesa. These may serve application reset functions
and are not counted as ordinary tone presets. The catalog therefore contains
3,673 other XML files, whose individual application role has not been
independently established. Discovery excludes the exact filename
`Reset All Settings.xml`, case-insensitively at every scanned depth, including
manually browsed folders. Parsing a file and finding a cab scalar does not by
itself establish that the file should be offered for editing.

## Numeric-ID cab section

Tom Morello and Mesa share a numeric parameter family. `400` precedes two
matching microphone groups beginning at `403` and `415`; their position,
distance, gain, pan, phase and mic-selection value patterns match two IR slots.
A separate `cabsims/cabsim` container stores `filePath1` and `filePath2`.
The Mesa variant additionally has cabinet selection `401` and per-cab mic choices.
All these settings and IR paths are preserved.

Recognition requires exactly one nonempty `cabsims` container, without properties
or non-whitespace text, containing only flat `cabsim` children with string
`filePath1`/`filePath2` properties and no non-whitespace text. Its parent must
also contain sibling `PARAM` identities
`400,403,404,405,406,407,408,409,411,412,415,416,417,418,419,420,421,423,424`.
Only `PARAM[id=400].value` is edited; that node must have exactly the `id` and
`value` properties, no children and no non-whitespace text. A standalone numeric
`400` is insufficient. Additional unrelated parameters and unseen product/version
metadata are allowed. Duplicate sibling IDs and competing recognized whole-cab
flags reject the input.

The factory catalog has `400` set to 1 in 297 files and 0 in three Tom Morello
files. A controlled Tom Morello on/off export comparison corroborates `400` as
the whole-cab flag with **1 enabled / 0 disabled**: both exports are binary
ValueTrees and the flag is a double. Mesa shares the guarded structure, but its
plugin behavior has not been independently verified. See
[validation and confidence](#validation-and-confidence) for the distinction
between comparison evidence and edit-integrity checks.

## Legacy named bypass section

NTS has 19 binary ValueTrees using `PARAM[id=cabsim_bypass].value` and 13 text XML
files using `Parameter@cabsim_bypass`. All have a value of 1, while
`cabsim1_enabled` and `cabsim2_enabled` separately control slots. Factory head
and pedal section values also use this bypass naming convention.

The assessed interpretation is that `cabsim_bypass` is a section-active control
with **1 enabled / 0 disabled**, despite its name. A literal bypass interpretation
would classify every audited NTS factory cab as bypassed. This polarity remains
provisional without a controlled whole-cab comparison or plugin test. Only the
exact `PARAM` identity or property on `Parameter` is recognized; there is no
general rule that reverses or edits every field named bypass.

## Validation and confidence

All **3,677/3,677** catalog XML files passed both-direction edit validation; none
were structurally unsupported. These checks used temporary copies, the native
C++ engine and an independent strict tree reader from the test-only Python
reference. Enable and Disable must reparse, report the requested state, retain
scalar types, preserve every byte before/after the selected scalar and preserve
every unrelated parsed value. Already-requested states remain byte-identical.
Original input bytes are checked after the run. These results establish edit
integrity across this catalog, not compatibility with every released plugin or
version, audible correctness or plugin-load acceptance.

Whole-cab comparison evidence covers the named `cabActive`, hierarchical
`cabParameters.sectionActive` and guarded numeric `400` layouts, as reflected in
the [README table](../README.md#supported-plugin-formats). A controlled comparison
loads a preset, saves an export, toggles only the whole-cab control, saves another
export and compares the results. Custom saved comparison exports are separate
from the factory catalog available through installers. A comparison confirms
the assessed mapping for its samples; it does not test every plugin listed under
the same layout.

For the Tom Morello comparison, switching off cabinet simulation was the only
user action in the plugin. The two exports differ in 15 scalar values: the cab
flag, nine other numeric parameters, two IR-path properties and three metadata
properties, with no added or removed parsed fields. Saving in the plugin can
produce such extra differences even for a toggle-only workflow. The editor
preserves every field except the cab scalar. All four file/direction combinations
passed requested-state, scalar-type, exact outside-span byte preservation,
unrelated-value preservation, idempotence and exact reverse-round-trip checks.

The NTS layouts lack a controlled comparison. Legacy Nolly is absent from this
factory catalog; its exact `PARAM[id=cabActiveID].value` support remains
provisional. The legacy Nolly export pairs leave that flag at 1 while changing
microphone-slot switches, so they do not demonstrate a whole-cab toggle. See
[format evidence](ADAPTERS.md#evidence-and-confidence). Neither filenames nor mic
states determine the whole-cab state.

Generated public tests cover numeric and legacy bypass structures in text XML
and binary ValueTree, unseen identities, formatted numeric strings,
reversibility, idempotence, missing anchors, mic-only files, duplicate IDs and
competing switches. These generated cases verify shared recognition and
scalar-preservation rules; they do not establish native acceptance in each
plugin.

To verify a local catalog without adding preset fixtures to the repository:

```sh
python scripts/audit_preset_library.py --backend /path/to/CabBackend \
  --presets /path/to/preset-catalog --report /path/to/factory-audit.json
```

The report records aggregate coverage rather than preset contents. Local preset
inputs and reports should remain outside the repository. Full recognition rules
and scalar-preservation behavior are documented in [ADAPTERS.md](ADAPTERS.md).
