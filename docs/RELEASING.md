# Releasing

Build locally in cloud using [cross-build instructions](CROSS-BUILDING.md); never use GitHub Actions. Commit application inputs first and record clean source, application version, architecture and SHA-256 in local build evidence.

Use the default prerelease profile for test candidates, reusing unchanged validation. Use `--profile full` for stable builds on both platforms, with Windows `--gcc-lto` (single partition) and macOS ThinLTO. After backend changes run native differential/workflow checks. Package gates include deployment/dependency inventory, macOS ad hoc signing and ZIP integrity. Windows must contain only the EXE at ZIP root, with complete embedded notices; macOS remains a zipped universal .app.

Create a draft release only after artifacts are ready; mark test candidates as prereleases and publish stable releases when authorized. Upload both packages, platform notices and checksums. Verify each remote asset size/digest once, then publish. Keep build-info JSON out of release assets and packages. Descriptions should state downloads, minimum targets and signing/test status concisely, not recount changes.

Publishing a stable release does not establish universal platform/plugin compatibility. Compatibility claims require actual Windows/macOS GUI, elevation and plugin loading checks. Wine and cross-build checks do not establish native compatibility.
