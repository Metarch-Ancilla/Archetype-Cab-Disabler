<p align="center">
  <img src="assets/cab-disabled.svg" alt="Archetype Cab Disabler logo" width="256" height="256">
</p>

<h1 align="center">Archetype Cab Disabler</h1>

Neural DSP wont give desktop Archetype users a global cab simulation bypass switch. Now you can do it yourself.

Enable or disable cabinet simulation across all your Neural DSP desktop plugin presets in one go. 

Amp, effects, microphone and IR settings stay as saved. You choose which plugins, folders and presets to change, including factory, artist or your own custom presets.

For desktop plugins only, not compatible with Quad-Cortex profiles.

## Download and open

[Download for Windows or macOS — 1.0.0](https://github.com/Metarch-Ancilla/Archetype-Cab-Disabler/releases/tag/v1.0.0)

| Platform | Requirements | Open after extracting the ZIP |
| --- | --- | --- |
| Windows | 64-bit PC | `Archetype Cab Disabler.exe` |
| macOS | Intel: macOS 10.13 or later. Apple Silicon: macOS 11 or later. | `Archetype Cab Disabler.app` |

Extract and run; no installation required.

Your computer may show a security prompt on first launch.
If macOS blocks opening, use **Open Anyway** in System Settings → Privacy & Security, or System Preferences → Security & Privacy on older Macs.

## How to use

Close your Neural DSP plugin before editing its presets.

1. **Select Plugins:** wait for indexing, then tick your plugins. Use **Browse** to add your Neural DSP preset folder in a non-standard location.
2. **Select Presets:** expand the folder tree and choose whole folders or individual presets. Select User presets if you want to include your own.
4. **Edit Mode:** choose **Enable** or **Disable**, then **Direct Edit** or **Copy and Edit**.
5. **Review:** check your settings and selected presets, then click **Apply** to begin.
6. **Results:** check the successful and failed counts and any listed reasons, then click **Finish** to close the app.

## Choose how to save your changes

| Mode | What happens |
| --- | --- |
| **Direct Edit** | Updates the original presets in their existing folders, keeping their names. |
| **Copy and Edit** | Keeps your originals and creates edited copies in each plugin’s **Cab Disabled** or **Cab Enabled** folder, according to your selected operation. Preset names and subfolders are preserved. Existing files are never overwritten. |

Try **Copy and Edit** first to keep your existing library intact. Both modes support **Enable** and **Disable**.

## Backups

We recommend creating a backup before using Direct Edit. It contains the original versions of presets due to be changed and is created when you click Apply, before editing begins. If the requested backup cannot be completed, editing stops.

To restore presets, close the plugin, extract the backup ZIP and copy the required files back to their original preset folders.

## Supported plugin formats

Factory presets used to assess these formats are publicly available through Neural DSP’s official desktop plugin installers.

| Layout / cab flag | Plugins observed using it | Tested |
| --- | --- | --- |
| Named parameter:<br>`PARAM[id=cabActive].value` | Archetype Abasi; legacy Archetype Gojira; Fortin Cali Suite; OMEGA Ampworks Granophyre | ✅ |
| Hierarchical section:<br>`cabParameters.sectionActive` | Archetype Cory Wong X, Gojira X, John Mayer X, Misha Mansoor X, Nolly X, Petrucci X, Plini X and Tim Henson X; Darkglass Ultimate; Fortin Nameless Suite X; Morgan Amps Suite; Soldano SLO-100 X; Tone King Imperial MKII X | ✅ |
| Numeric parameter:<br>`PARAM[id=400].value`, identified by the surrounding cab/IR structure | Archetype Tom Morello; Mesa Boogie Mark IIC+ Suite | ✅ |
| Legacy named parameter:<br>`PARAM[id=cabActiveID].value` | Legacy Archetype Nolly | ❌ |
| Legacy named parameter:<br>`PARAM[id=cabsim_bypass].value` | Fortin NTS Suite | ❌ |
| Legacy attribute:<br>`Parameter@cabsim_bypass` | Fortin NTS Suite | ❌ |

✅ means the layout was validated by comparing otherwise identical cab on/off exports.<br>
❌ means that comparison has not been validated for the layout, not that it is unsupported.<br>
A tick does not establish testing of every listed plugin or version.

## If something needs attention

**A plugin is missing:** use Browse on Select Plugins and choose either your Neural DSP presets folder, or the folder named after your plugin; this may contain Presets subfolders. Wait for indexing to finish, then check the list again.

**Administrator access is requested:** some original presets are stored in protected folders. Direct Edit asks for permission when needed after you click Apply. Copy and Edit keeps the originals intact and does not request administrator access.

**Some presets were skipped or failed:** read the reason shown in Results. Presets already in the requested state need no changes. Others may use an unsupported format or be unavailable for editing. Unsupported formats are left unchanged.

## Further information

See the [feature reference](docs/FEATURES.md) for supported behavior and safety details, or the [build instructions](docs/CROSS-BUILDING.md) for development information.

## License

Copyright © 2026 Metarch-Ancilla. [MIT](LICENSE). Third-party notices are available through **Licenses** in the app.

Independent project; not affiliated with Neural DSP.
