# Expat source provenance

Expat 2.9.0, upstream release `R_2_9_0`:
https://github.com/libexpat/libexpat/releases/tag/R_2_9_0

Source archive: `expat-2.9.0.tar.xz`, verified against the GitHub release asset
SHA-256 `1e6371862cc31999b368c3b89b49994f0677e1bab5f1b2b85ae3741f5d803051`.
Only library files required by our configuration are retained, unchanged from
upstream, with `COPYING`. `expat_config.h` is the project's local configuration.

Namespace expansion, external DTDs, general entities and diagnostic input-context
buffering remain disabled. The backend rejects every DOCTYPE, enforces byte,
depth and event limits, validates literal cab spans and supplies 128 bits of
operating-system entropy through `XML_SetHashSalt16Bytes` before parsing.
Windows also compiles upstream's required `random_rand_s.c`; other targets do
not enable Expat's optional OS entropy modules. No Python APIs/runtime used.
