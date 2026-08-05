# Contributing to QuickImageViewer

Thank you for your interest in improving QuickImageViewer. I prioritise clean,
explicit and performant code. Please read the licensing section before opening a
pull request — it is short, and it is the part that cannot be fixed afterwards.

## Licensing of contributions

QuickImageViewer is released under the **GNU Affero General Public License v3.0**,
and it will stay that way.

It is also maintained by a single author, which makes it possible to offer the same
code to commercial users under separate terms. Keeping that possible requires one
thing from contributors, so it is asked for plainly:

> **By submitting a contribution, you grant Ivan Hristov Yanev a perpetual,
> worldwide, non-exclusive, royalty-free, irrevocable licence to use, reproduce,
> modify, publish and distribute your contribution, and to sub-license it under
> other terms, including commercial ones. You confirm that you wrote it, that you
> have the right to submit it, and that you are not knowingly including anyone
> else's copyrighted work.**
>
> You keep the copyright in what you wrote. This grants a licence; it does not
> transfer ownership.

Your contribution is released to everyone else under AGPLv3, exactly as the rest of
the project is. Nothing you contribute becomes closed to the public.

**Why this is asked for.** Without it, relicensing any file you touched would need
your written permission years later, and in practice that means it can never be
relicensed at all. A single merged pull request without this grant permanently
removes that option for that file.

If you are not comfortable granting it, that is entirely reasonable — please open an
issue describing the change instead. A well-written issue is genuinely welcome and
is often more useful than a patch.

### Sign your commits off

Add a `Signed-off-by` line to each commit, which `git commit -s` does for you:

```
git commit -s -m "Fix HEIC orientation handling"
```

This is the [Developer Certificate of Origin](https://developercertificate.org/) —
your statement that you have the right to submit the code. Pull requests without it
will be asked to add it before review.

## How to contribute

1. Fork the repository and create a feature branch.
2. Make the change.
3. Open a pull request with a short description of what it does and why.

## Code style

Keep it simple and explicit. Avoid unnecessary abstraction. Straightforward
top-to-bottom control flow is preferred over clever indirection — if a reader has to
work out the order of operations, the code is doing too much hiding.

New source files should carry the standard header:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
```

## Where things live

| Area | Files |
|---|---|
| Image decoding and format support | `src/WicDecoder.cpp`, `src/SimpleFormats.cpp`, `src/SvgDecoder.cpp` |
| Rendering and viewport | `src/Renderer/RendererD2D.cpp` |
| File I/O | `src/Platform/FileHandler.cpp` |
| Commands and input | `src/Input/` |
| Remote control and mirroring | `src/Rem_TCP_IP/` |
| Panels and windows | `src/UI/` |
| Settings and persistence | `src/Persistence/` |

Please keep a pull request focused on one area. A change touching the renderer, the
remote protocol and the settings system at once is very hard to review, and usually
turns out to be three changes.

## What is unlikely to be merged

- Telemetry, analytics or any phone-home behaviour, in any form.
- New third-party dependencies, unless they replace more code than they add.
- Reformatting passes across files that are not otherwise being changed.

I look forward to reviewing your contributions.
