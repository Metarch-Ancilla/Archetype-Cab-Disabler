# Test-only reference implementation

`cabdisabler/` is a frozen Python differential test oracle. It is never an
application entry point or part of either distributed package.

`tests/__init__.py` and `scripts/validate_cpp_backend.py` explicitly add this
reference path for generated regression cases. Production logic lives exclusively
in `native/backend/`.
