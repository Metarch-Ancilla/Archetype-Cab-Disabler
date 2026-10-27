$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
# Native build: CMake + Visual Studio C/C++ toolchain; no Python runtime.
cmake -S frontends -B build/native-windows -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build build/native-windows --config Release
exit $LASTEXITCODE
