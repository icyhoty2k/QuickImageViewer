# miniz — vendored, and vendored SEPARATELY

Upstream: https://github.com/richgel999/miniz — MIT.
Version here: **11.3.2** (`MZ_VERSION` in `miniz.h`).

## Why it is its own directory

It used to be the single amalgamated `miniz.c`/`miniz.h` bundled inside
`libs/lib/tinyexr/include/`. That coupled two unrelated update schedules: miniz
could not be refreshed without touching tinyexr, and the bundled copy had drifted
to **11.0.2** — three minor versions behind — on a decompressor that reads
untrusted input from two directions (EXR scanlines, and Backup archives).

Split on 2026-08-06. tinyexr keeps working unchanged: it does
`#include <miniz.h>` and calls only `mz_compress` / `mz_uncompress`, an API that
has not changed.

## Who uses it

| Consumer | API used |
|---|---|
| `libs/lib/tinyexr/include/tinyexr.h` | `mz_compress`, `mz_uncompress` — EXR ZIP scanlines |
| `src/UI/AppMenu/AppMenuIO.cpp` | heap ZIP writer/reader — Backup History & Favorites |
| `src/GeoNames.cpp` | `mz_uncompress` |

`src/AppMain.cpp` includes the header but calls nothing — harmless, and left
alone rather than churned.

## Two local additions

**`miniz_export.h` is ours.** Upstream generates it with CMake's
`generate_export_header()` to fill `MINIZ_EXPORT` with dllexport/dllimport. qIV
compiles miniz's sources straight into the exe, so the macro expands to nothing.
Vendoring nine lines beats making this build depend on miniz's build system.

**`MINIZ_NO_STDIO` is defined target-wide** in `CMakeLists.txt`. It compiles out
every file-path API, which removed a CodeQL time-of-check/time-of-use alert
(High) in `mz_zip_add_mem_to_archive_file_in_place_v2`. Nothing above needs those
APIs — all three consumers work on memory. It is self-enforcing: reaching for a
file-based miniz API now fails to compile.

## Updating

Drop in the new `miniz*.{c,h}` from upstream, keep `miniz_export.h`, and check:

1. `MZ_VERSION` moved.
2. `miniz_export.h` still only needs `MINIZ_EXPORT` / `MINIZ_NO_EXPORT`.
3. The three consumers above still compile — `MINIZ_NO_STDIO` will catch a new
   file-API dependency at compile time rather than at runtime.

## Known CodeQL alert that an update does NOT fix

`tinfl_decompress` computes `(pOut_buf_next - pOut_buf_start)` for
`out_buf_size_mask` **before** validating those pointers for null. Flagged as
"redundant null check due to previous dereference".

**Present in upstream `master` as of 11.3.2 — checked, not assumed.** No caller in
qIV can pass null: `mz_uncompress` always supplies a real buffer and nothing calls
`tinfl_decompress` directly. Dismiss the alert; do not patch vendored source to
satisfy a scanner, because that means owning a fork of miniz forever.
