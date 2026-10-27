#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
# Cloud cross-build: Python build tooling, SDK and cached Clang/LLD required.
exec .venv/bin/python scripts/cross_build_macos.py "$@"
