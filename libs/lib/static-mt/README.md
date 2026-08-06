# static-mt — libraries built against the STATIC CRT (/MT)

Used only when the build is configured with `-DQIV_STATIC_RUNTIME=ON`. With the
option off (the default) nothing here is read and `../static/` is used instead.

A static `.lib` records which C runtime it was compiled against, so the two
cannot be mixed: linking the `/MD` copy into an `/MT` binary fails with

```
LINK : warning LNK4098: defaultlib 'MSVCRT' conflicts with use of other libs
openjp2.lib(opj_malloc.c.obj) : error LNK2001: unresolved external symbol
                                __imp__aligned_free   (also _malloc, _realloc)
```

The `__imp_` prefix means "imported from a DLL" — under `/MT` there is no CRT
DLL to import from.

## What belongs here

Only the libraries that actually need an `/MT` variant. `CMakeLists.txt` falls
back to `../static/` per library, so a file that is absent here is simply taken
from there.

| File | Needed? |
|---|---|
| `openjp2.lib` | **Yes** — the `/MD` copy is what blocks `/MT` |
| `resvg.lib` | Not so far — the `/MD` copy links clean under `/MT` |

## Producing openjp2.lib

**It must be OpenJPEG 2.5.4.** The headers in `../openjpeg/include` are shared by
both variants, so a different version compiles against one set of structs and
links against another — an ABI mismatch that builds cleanly and corrupts memory
at run time. Check with `vcpkg list openjpeg` before copying.

```
vcpkg install openjpeg:x64-windows-static
copy <vcpkg>\installed\x64-windows-static\lib\openjp2.lib  openjp2.lib
```

`x64-windows-static` is the triplet that means *static libs **and** static CRT*.
Do not use `x64-windows-static-md`, which is static libs with the dynamic CRT —
the exact combination that does not work here.

From source instead:

```
cmake -DBUILD_SHARED_LIBS=OFF -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ..
```

## Why bother with /MT at all

Two reasons, and the second is the larger one:

1. Three fewer DLL loads per launch (`msvcp140`, `vcruntime140`,
   `vcruntime140_1`) — roughly 0.3–1 ms, on the cold path and on every
   "open with qIV" while a copy is already running.
2. **No VC++ redistributable dependency.** *"The code execution cannot proceed
   because VCRUNTIME140.dll was not found"* is a real first-run failure for a
   portable app downloaded by someone who has never installed one.

Cost: roughly 200–500 KB of exe, and CRT security fixes need a rebuild instead
of arriving through Windows Update.
