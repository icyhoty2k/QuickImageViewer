# Remote mirroring — design record

Companion to `REMOTE_TCP_IP_SPEC.md`, which covers the wire protocol and the
listener. This covers the layer built on top: **one qIV driving others**.

> **Scope warning.** This documents the mirroring/observing work and the two
> refactors that enabled it. It does **not** cover `MirrorPicker.cpp/.h`
> (target selection at F11 time, Shift+F11, `MirroredLiveCount()`), which was
> added separately. Read that file's own header comment for it.

---

## 1. What it does

Two instances, or several. One drives; the rest follow.

| Key | Meaning |
|---|---|
| **F11** | Mirroring on/off — forward my commands to my targets |
| **F12** | While mirroring, also execute here (off = pure remote control) |
| **F10** | Remotes console — the instances this copy drives |
| **F9** | Remote Server — the listener *this* instance runs |

Both F11 and F12 are **session-only and start OFF at every launch**. A viewer
that came back from a restart already driving machines you'd forgotten about
would be the worst kind of surprise.

**Observing** is the other direction: the F10 console's ◉ cell asks a target to
report what it does, and this viewer follows along — how you watch a screen
running its own slideshow. Exclusive: one at a time, because two would
interleave two unrelated streams of actions into one screen.

---

## 2. The command sink — why everything routes through `ExecuteCommand`

**The single most important invariant in this design.**

Every input path resolves to a `Command` and goes through
`InputManager::ExecuteCommand` — keyboard, mouse, tray, context menu, panel
clicks, and the socket. Nothing applies a side effect by calling
`LoadImageIndex` or an `AppCommands` helper directly.

That is not tidiness. `ExecuteCommand` is where mirroring is gated, so **an
input path that skips it is an input path that silently does not mirror.**

This required refactoring the mouse (`MouseHandler.cpp`), which previously
duplicated behaviour that already existed as commands — wheel nav, wheel zoom,
double-click fullscreen, XButton1, RMB+LMB reveal, MMB reset, H-wheel history
walk. Each was byte-identical to the matching `case` in `CommandExecuter.cpp`.

Commands created to make that possible: `ResetWindowLayout` (MMB — *not*
`ResetAll`, which also clears effects), `OpacityUp`/`Down`,
`Prev`/`NextHistoryFolderAll` (the wheel's `WalkScope::All`, a third scope).

**Deliberately still direct:** continuous gestures (window drag, viewport pan,
MMB resize) — a stream of pixel deltas is not a discrete action. And the
slideshow timer, which calls `LoadImageIndex` directly *and should*: routing it
through the sink would forward `next` to slaves running their own slideshows.

---

## 3. Three policy tables (`RemoteProtocol.cpp`)

All three answer questions about the same enum, in one file, on purpose.

### `NEVER_REMOTE` — structural
Delete, move, paste, copy-files, save. **Must be impossible to drive over a
socket under any configuration.** Enforced twice:

- a `consteval` + `static_assert` proves none has a row in the command table —
  give one a row and **the build fails**;
- `ExecuteOnUiThread` refuses them again at run time.

Why so strong: authentication happens **once**, at connect
(`RemoteServer::Authenticate`). Lines after it carry no per-message signature,
so on a routable network anyone who can inject into an established session
issues commands as the authenticated caller. Better auth would narrow that;
keeping `delete` off the menu removes it.

### `SESSION_BLOCKED` — policy
Refused *locally* while any connection is live, each entry carrying its reason:

- `SortByDisk` — physical disk order differs per drive
- `FindImage` — searches this playlist only
- delete / move / paste / `SaveImage` — change the file set, shifting every
  index after the change

Checked via `Remote::BlockedNow(cmd, reason)` at the **top** of
`ExecuteCommand`, before the mirror gate, so a refused command neither runs nor
travels. Always says what it dropped and why.

**Two callers**, and that is deliberate. `ExecuteCommand` covers every command
path. `ThumbnailPanelWnd`'s file operations are the exception: they act on the
hovered item or selection, carry cut state between calls, and arrive by
drag-and-drop as well as menu and keyboard — so they are not commands and can't
become them without moving that state out of the panel that owns it. They ask
the same table at their five existing gates.

### `IsMirrorable` / `IsMirrorableRemote` — what fans out
**Not the same question as "is it in the command table."** That table is the
*scripting* surface and is deliberately permissive — a script legitimately wants
`quit`, `HideToTray`, geometry. Mirroring is a keystroke fanned to every screen
at once, where the same commands are wrong.

Denied: `HardQuit`, `HideToTray`, `NewWindow`, `ShowInExplorer`, `SaveImage`,
`CopyToClipboard`, all panel toggles, all geometry, `CmdArgs*`,
`ToggleDedicated`, and the mirror controls themselves. Anything refused stays
reachable individually.

---

## 4. The loop cut (`RemoteInbound.h`)

Master mirroring to a slave that is also observing the master:

```
master presses Space → forwards "next" → slave executes → slave echoes
"EVENT next" → master executes → master forwards "next" → …
```

One keystroke, forever, at socket speed. Two instances observing each other do
it with no master at all.

`InboundGuard` (RAII, UI thread, scoped so an early return can't leave it stuck)
suppresses **both** forwarding and echoing for anything that arrived from the
wire. `InboundSource()` carries *which* connection, because suppression isn't
all-or-nothing: a command from observer A must still reach B and C.

`ForwardGuard` solves a second double-send: the gate forwards a *command*
(`next`), and `LoadImageIndex` forwards a *position* (`goto 47`). Both are
needed — most picture changes aren't commands — but for a navigation command
both would fire. Sending both is worse than redundant: `next` means "advance in
YOUR playlist", and a `goto` behind it overwrites that with a position from a
different list.

---

## 5. Same machine vs another machine

Decided **once**, at `AddTarget`, by resolving the host and checking it against
every address this machine answers to — so a local instance addressed by LAN IP
or computer name is correctly treated as local.

| | Same machine | Another machine |
|---|---|---|
| Commands | full mirrorable set | portable subset |
| `goto <n>` positions | yes | **no** — index means nothing against different files |
| `open <path>` | yes | **no** — drive letters don't carry |
| `folder=` in `sync` | yes | **no** |
| Divergence check | yes | **no** — different names are normal there |
| Console's start button | works | can't — nothing launches a process remotely |

A remote instance is a **parallel viewer** running the same actions over its own
content, not a mirror. That's the most that can honestly be delivered.

Observers are classified the same way, at `observe 1` time, by `getpeername`.

---

## 6. Positions, and why indices

Item picks (thumbnail, Find, JumpTo, history) forward as a **1-based index**,
not a path. A path would mean a filesystem round trip on the UI thread for every
thumbnail click; an index costs nothing and stays meaningful because **sort
order is itself a mirrored command**.

Identical sort isn't proof of identical indices — the two ends must also hold
the same *file set*. So every reply names the file actually landed on
(`OK goto=47/238 IMG_0042.jpg`), the sender compares, and a mismatch posts
`WM_QIV_REMOTE_DESYNC` → the UI thread pushes `sync` and resends. Divergence is
detected and repaired rather than assumed impossible.

`SortByDisk` is refused while connected precisely because it is *not*
reproducible across drives.

---

## 7. `sync` — the drift cure

Mirroring forwards **toggles**. A toggle applied to a different starting state
inverts instead of matching, and the effect *chain* is ordered — two instances
can hold identical flags and draw visibly different images.

Preventing this would need a payload command per toggle. Instead `sync` pushes
the whole state: sort, view mode, rotation, flips, gamma/brightness/contrast/
saturation, the effect list **in order**, overlay state, slideshow settings, and
the folder (same-machine only). Built and parsed in the same file
(`RemoteExec.cpp`) so a key can't be spelled differently at each end.

Deliberately carries **no playlist position** — applying a folder starts an async
scan a position would race. Position travels separately.

---

## 8. `qivRemotes.ini`

```
[Remotes]
1=Name,IP,Port,Password,AutoConnect,ExePath
```

Beside the exe, and **deliberately not the exe-derived name** — an `.ini` called
`qIV.ini` next to `qIV.exe` is what flips the whole app from registry-backed to
file-backed (`Dedicated::DetectStartupMode`). This name is invisible to that
check.

**The name is the identity.** Enforced in three places: `LoadRemotes` drops a
duplicate name; adding refuses a name collision (and separately a host+port
collision — different mistake, different message); `PersistRows` matches by name
so correcting a row's port keeps its password.

`Password` holds either a typed password or `secret:<salt>$<digest>` — an
imported credential. The prefix exists so the two are distinguished explicitly
rather than guessed from shape.

**Import** (`ImportFromInstanceFile`): point at an instance's `.ini` *or* `.exe`
(either finds the other by extension swap) and get port, name, exe and
credentials with nothing to type. The credentials work because **the digest is
the shared secret** — anything that can read that file can already
authenticate, so importing grants nothing new. Hard-fails on: no `.ini` beside
the exe, no `[REMOTE_TCP_IP]` section, no valid port, malformed password.
Warns (offering to continue) on: no Name, `Enable=false`, empty or
non-loopback `AllowList` — the last three are invisible from outside and would
present as a timeout.

`AllowList` **seeds** to `127.0.0.1`. Seed only — the accept gate gives it no
special status, so deleting it locks this machine out like any other entry. A
list with built-in exceptions is not a list.

---

## 9. Standalone cost (short-circuits)

A viewer using none of this must pay nothing measurable.

- **Per command:** four loads, one cross-TU call. No lock, no allocation, no
  table walk. Ordered so `IsMirrorable`/`NameForCommand` are never reached.
- **Per image change:** three loads. The `goto` line and filename `substr` are
  built *only* if something is listening.
- **Startup:** one `GetFileAttributesW`. No thread, no socket, no Winsock.

Two fixes were needed to get there: `HasObservers()` was taking a **mutex** on
every keystroke to find an empty vector (now an atomic count), and
`LoadImageIndex` built strings **before** checking anything.

**`SessionActive()` counts CONNECTED targets, not configured ones.** Using
configured meant a copy that merely *had* a `qivRemotes.ini` sat in restricted
mode forever. Likewise the mirror gate checks `HasLiveTargets()` before
`IsMirrorable` — F11 stays on across a target dropping (it reconnects by
itself), and without that check the table walk happened per keystroke for as
long as nothing answered.

---

## 10. Decisions taken and *not* to re-propose

- **Gate lives inside `ExecuteCommand`, not a `Dispatch()` wrapper.** A wrapper
  would leave every existing direct caller silently un-mirrored — the exact bug
  the refactor eliminated.
- **Toggle drift is cured, not prevented.** `sync` on demand beats a payload
  command per toggle.
- **Password stored as typed in `qivRemotes.ini`.** User's call, reasoned: local
  single-user box, loopback listeners. Pairs with the loopback binding.
- **No `sharedPaths` opt-in** for NAS-backed machines. Locality alone decides.
- **F9 = server only, F10 = client only.** F9 is what others connect to; F10 is
  what this connects to.
- **Free-text "Send command" was removed**, not relocated. The protocol is plain
  text so `netcat` is a first-class client; hand-typing belongs there.

---

## 11. Two refactors this work required

**`src/UI/AppMenu/`** — the menu was split build-here / execute-there:
structure in `ContextMenuHandler.cpp`, behaviour in `TrayHandler.cpp`, which was
**93% menu dispatch** in a file named after the tray. Now six files under one
folder. The real win: twenty id constants that said *"values must match the
cases in TrayHandler::DispatchCommand"* are one enum both halves include, with
`static_assert`s enforcing the band arithmetic. `TrayHandler` is 55 lines.

**`src/UI/GdiPool.*`** — every panel created and destroyed its brushes and pens
per paint, and panels repaint on mouse-move to track a hovered row: ~30 handles
per paint, hundreds to drag down a list. Now pooled by colour, flushed on theme
change. **Callers must not `DeleteObject` what they get back.**

---

## 12. Known gaps

- **Cross-machine security.** Plaintext protocol; connection-time-only auth with
  no per-message MAC. Fine on loopback; on a network, treat the network as the
  boundary (VPN/trusted VLAN) or add per-line HMAC.
- **No `SO_KEEPALIVE`** on client sockets — an unplugged machine leaves a
  half-open socket and the dot stays green until the next send fails.
- **Request/reply serialises.** At LAN latency, holding an arrow key can outrun
  the queue (bounded, drops oldest).
- **`MirrorPicker` is not covered here** — see §0.
- **Untested as of writing**: the F10 console's layout arithmetic, the ini
  import path, and everything cross-machine.
