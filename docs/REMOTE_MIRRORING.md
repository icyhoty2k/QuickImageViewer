# Remote mirroring — design record

Companion to `REMOTE_TCP_IP_SPEC.md`, which covers the wire protocol and the
listener. This covers the layer built on top: **one qIV driving others**.

> **Scope warning.** This documents the mirroring/observing work and the two
> refactors that enabled it. It does **not** cover `MirrorPickerWnd.cpp/.h`
> (target selection, Ctrl+F11, `MirroredLiveCount()`), which was added
> separately. Read that file's own header comment for it.
>
> That selection UI used to be a checkable popup put up by F11 itself
> (`MirrorPicker.cpp/.h`, now deleted). It is a floating panel on **Ctrl+F11**:
> F11 is the toggle and nothing else, and the panel can be left open while
> mirroring goes on and off.

---

## 1. What it does

Two instances, or several. One drives; the rest follow.

| Key | Meaning |
|---|---|
| **F11** | Mirroring on/off — forward my commands to my targets |
| **Ctrl+F11** | Remotes Control — which connected instances F11 drives |
| **F12** | While mirroring, also execute here (off = pure remote control) |
| **F10** | Remote Servers — the instances this copy can drive |
| **F9** | Local Server — the listener *this* instance runs |
| **Ctrl+F12** | RemoteLog — every line that crossed the wire (recording off by default) |
| **Ctrl+Enter** | Push THIS picture to the instances under Control, without disturbing what they are doing — see §7b |
| **Alt+Enter** | STREAM this picture there, shown once, changing nothing at all — any machine — see §7c |
| **Ctrl+Alt+Enter** | Ask one instance for the picture it is displaying and show it here, once — any machine — see §7c |

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
clicks, and the socket. That includes the **payload** commands, which until
recently had **three** call sites reaching `Remote::ExecutePayload` directly and
skipping the sink: the server dispatch, AppMain's observer replay, and the
Ctrl+F10 Send Command panel's "also run it here". None of them detoured because
the sink could not do the work — it calls the same handler — but because the
payload overload returned `void` and each needed the reply line.

It hands that back now (`replyOut`, and a `bool` saying whether a payload handler
ran), so there is nothing left to route around. All three call it, and the two
things attached to the sink that payload commands were missing — the crash
breadcrumb and the observer echo — now cover them. `ExecutePayload` has exactly
one caller, inside the sink, which is what keeps that true.

The constraint the detours existed to honour still holds and lives inside the
overload: a payload command runs its **headless** handler and never reaches the
bare switch, where several raise a panel or a dialog that would hold a socket or
a keypress open until somebody dismissed a window.

Two kinds of side effect are reached without a command, both deliberately, and
neither is an input path:

- **`LoadImageIndex`**, which is where a *position* is announced. Item picks
  (thumbnail, Find, JumpTo, history) travel as an index from there rather than as
  a command — §6 — and the slideshow timer reaches it with no command at all.
  `ForwardGuard` (§4) is what keeps the two announcement points from both firing
  on one keypress.
- **`ThumbnailPanelWnd`'s file operations**, for the reason given under
  `SESSION_BLOCKED` below.

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
Refused *locally* while this instance is sending **unsolicited positions**, each
entry carrying its reason. Two relationships do that: a live mirror target, and a
**same-machine observer** (the only kind a positional `goto` is sent to — §5).

This used to read "while any connection is live", and the code matched — it asked
`ActiveConnections()`, every client the local server had accepted. That put a
connected **phone** into the test, so opening the Android app took Delete, Move,
Paste, Save and Find away from the keyboard, each with an overlay citing a
mirroring hazard that was not occurring. A client that asks and reads a reply
holds no playlist to keep aligned.

**A command that arrived from the wire is never refused here.** Every reason
below is about an index this instance *sends out*, unsolicited, with nothing at
the far end able to check it. A caller that asked gets back the file actually
landed on (`OK goto=47/238 IMG_0042.jpg`) and repairs its own drift — the
mechanism §6 describes. Without this the filter could not be applied to the wire
at all: it would refuse the phone's `FindImage` for searching the one playlist it
was asking about.

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

### `IsAnnounceable` — what a WATCHER is told
A fourth question over the same enum, sharing `IsMirrorable`'s body so every
safety exclusion above applies unchanged — an observer *executes* what it
receives, so a command wrong to fan out is equally wrong to push at a watcher.

It differs in one place: the table rule. `IsMirrorable` ends by refusing every
`PayloadRule::Required` row, because `Mirror::Broadcast` sends a bare name and
the value does not exist yet at the keystroke. An echo has no such problem — it
reports something already done, from a payload it was handed. Asking the mirror
predicate instead therefore silenced **every payload command for every
observer**: `ZoomTo`, `SlideshowSetInterval` and `OpenFile` announced nothing.
Invisible with one client; obvious with two, where the phone that acted knew from
its own reply and the other went stale.

Also removed as not-events: `SendDisplayedImage` / `SendDisplayedPreview` (read
only), and `StreamImageBegin` / `Chunk` / `Show` (transport framing — the picture
going up is announced by `ShowInterjectedImage`, §7c, the one point that also
covers the slide-boundary case where no command is in flight).

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

## 7b. Ctrl+Enter — pushing one picture, without disturbing the screen

`sync` lines the screens up by **replacing** state. Ctrl+Enter is the opposite
instrument, and both exist because the two jobs are different.

**The job:** choose a picture on the driving copy, press Ctrl+Enter, and the
instances under Control show it *while carrying on with whatever they were
doing*. A target running a fullscreen slideshow keeps running it — same view
mode, same effects, same interval — and simply continues from the pushed image.

So the push sends **folder, sort order and position, and nothing else**. `sync`
was considered for this and rejected: it also carries view mode, rotation, flips,
gamma/brightness/contrast/saturation, the effect chain, overlay state *and*
slideshow interval/loop/shuffle, so it would stamp the driving viewer's whole
look over a screen that was deliberately set up differently.

### It asks before it sends

`QueryState` (`Command::QueryState`, `RemoteProtocol.cpp`) is the **only
read-only row in the command table** — every other command reports its value as
a by-product of doing something. It answers
`count`, `index`, `sort`, `sortrev`, `name`, `folder`.

Per target, on **that target's own sender thread** — the one thread allowed to
wait for a reply:

1. `QueryState`.
2. Same folder, same order, long enough list → **`JumpToImage <n>` alone.** One
   round trip, no rescan, no flicker. This is what makes Ctrl+Enter usable
   repeatedly: walk a folder here, push each picture as you reach it.
3. Otherwise send only what differs — `OpenFile <folder>`, then the sort
   command — then **wait for the far end's async scan**, re-asking `QueryState`
   until it reports that folder with a list long enough for the index
   (`PUSH_SETTLE_TRIES` × `PUSH_SETTLE_MS`, Constants.h). The index goes **last**.

Step 3 is why a position was never put in `sync`: opening a folder answers the
moment the open is *accepted*, not when the playlist exists, so an index sent
into that gap indexes the old list. The push closes the gap by asking instead of
assuming.

**`SortBy*` are toggles, not setters** — the command for the order you are
already in flips ascending/descending. The presses are therefore computed from
the state observed in step 1: one press to change order (which lands ascending),
a second only if reverse is wanted; a single press to flip direction alone.

### Falling back to a path

Three cases cannot trust an index, and all three degrade to
`OpenFile <full image path>` — one line that opens the folder *and* lands on the
file through the far end's own scan:

- the far end did not understand `QueryState` (an older build);
- this viewer is sorted by **disk order**, which reproduces on no other drive
  (and which a live session already refuses — §3);
- the reply names a **different file** than the one pushed, i.e. the two
  playlists hold different file sets. Repaired by naming the file outright rather
  than by the heavier `sync`-and-resend the mirror path uses.

### The parts that are deliberate

- **Same-machine targets only**, like every other position — and skipped ones are
  **counted and reported**, not dropped silently.
- **Independent of F11**, like Sync now: it is an explicit act, and a viewer with
  mirroring off is the case it exists for. It *does* respect the Ctrl+F11 Control
  ticks, so pushing and mirroring cannot reach different screens.
- **No table row for `PushImageToRemotes`.** A slave told to perform it would push
  its own picture back at whoever asked.
- **One queue for lines and pushes**, so a push cannot overtake the keystrokes
  queued ahead of it.
- **The shuffle permutation is rebuilt when the playlist size changes**
  (`AppMain.cpp`, slideshow timer), started at the picture on screen. It was built
  once at slideshow start, so a folder replaced underneath a shuffled slideshow
  left it walking indices into a list that no longer existed.

---

## 7c. Alt+Enter / Ctrl+Alt+Enter — streaming one picture, either way

Three keys, one question at three depths:

| Key | What travels | Works across machines | The far end |
|---|---|---|---|
| **Ctrl+Enter** | a POSITION (folder, sort, index) | no — same machine only | GOES there and stays |
| **Alt+Enter** | the IMAGE BYTES, outbound | **yes** | shows it once, unchanged otherwise |
| **Ctrl+Alt+Enter** | the IMAGE BYTES, inbound | **yes** | is only read from |

Ctrl+Enter **moves** the far end to a picture. The two streaming keys **show** a
picture and leave nothing behind.

### Why bytes, and what exactly travels

A path is worth nothing on a machine that cannot read it, and these two exist
precisely for the screen in another room. So the picture's **own file bytes**
travel, base64-encoded, and the far end decodes them with its own WIC — as if it
had opened the file.

The FILE's bytes, not its pixels: a decoded frame is ten to fifty times larger
than the JPEG it came from, and every end already owns a decoder. The file **name**
rides along for exactly one reason — its extension selects that decoder. It is
never treated as a path.

### The plumbing is the point

This is deliberately built as protocol, not as an internal shortcut, because the
next client is **an Android app**: send a picture to a screen, or ask a screen what
it is showing. Everything here is implementable by any client:

```
StreamImageBegin <totalBytes> <fileName>    → OK <totalBytes>
StreamImageChunk <base64>                   → OK <received>/<declared>     (× n)
StreamImageShow                             → OK <bytes>;<name>;shown|queued

SendDisplayedImage        → DATA <base64>            (× n, body lines)
                            OK SendDisplayedImage=<bytes>;<name>
```

Outbound is a **sequence of ordinary commands** because a request cannot be
multi-line; inbound needs no framing because a **reply** can be (the `help` verb
already is). The declared byte count is what lets a receiver *prove* the transfer
arrived whole — a phone on a flaky link is an expected caller, and half a JPEG
decodes to a torn picture rather than to an error. Chunks are refused the moment
they exceed the declaration, so that number is also the only bound on the buffer.

`MAX_LINE_LEN` rose from 4 KB to 256 KB to hold one chunk; still bounded, still
per-connection. `Log::Add` now clips the two peer-controlled fields to
`LOG_LINE_MAX`, so a transfer does not put megabytes of base64 into the wire log.

### `ShowImageOnce <path>`

The path form is kept: on one machine it costs nothing, and it is the honest
spelling for a script. It assembles into the same mechanism.

### What the receiving end does

All three routes — the path form, an assembled stream, and Ctrl+Alt+Enter's answer
— land in ONE piece of state (`AppState::Interjection`, `FileHandler.cpp`,
`RemoteExec`):

- the path is made the renderer's **active bitmap through the path-keyed cache** —
  the Dedicated *promotion* trick. `app.playlist`, `app.currentIndex`, the sort
  order and the async pipeline are untouched, so there is nothing to put back;
- **when** it appears is decided at arrival: a running slideshow gets it at the
  **next slide boundary** (so the slide on screen is not cut short) and it
  occupies exactly one slide; a still viewer shows it immediately, or the instant
  its decode lands, and it stays until something else changes the picture;
- it is retired by **`LoadImageIndex`** — the one function every image change
  passes through — so "shown once" needs no timer of its own. Retiring **evicts
  it from the VRAM cache**: a one-shot advert must not go on holding memory, nor
  be served instantly to a later probe for the same path;
- a newly arrived interjection **replaces** one not yet shown. A queue would
  deliver adverts minutes after they were sent;
- a STREAMED picture is written to a **temp file** so the existing decode/cache/
  display path is reused unchanged, and the interjection **owns** that file:
  retiring it deletes the file as well as evicting the bitmap. Ctrl+Alt+Enter's
  answer is the same, with `immediate` set — the user asked for it at this
  keyboard, so it does not wait for a slide boundary the way an arriving advert
  does;
- the read and the base64 work happen on a **sender thread**, never the UI thread:
  the UI thread hands over a path and is given back a path.

`SendDisplayedImage` answers with what is **on screen**, which is not always a
playlist entry — an instance already showing an interjection reports that. Its one
blocking call, reading the file on the UI thread, is accepted knowingly: bounded by
`STREAM_MAX_BYTES`, only on explicit request, and the alternative breaks the
one-request-one-reply shape that makes the protocol implementable in fifty lines.

**Why not a lighter `OpenFile`:** `open` *joins* the file to that viewer — it
rebuilds the playlist, moves the index, and the folder becomes where that
instance lives. Every one of those is the thing this feature must not do.

Two paint-path guards make it hold still: `WM_QIV_REPAINT` must not activate the
current playlist entry while an interjection (or a promotion) is up, or any
landing decode rips it off the screen — the bug that once made promotions flash
and vanish. The queued-but-undecoded case is checked **before** the `wParam == 1`
early return, because an interjection is warmed through the neighbour-preload
path and its arrival *is* a `wParam == 1` message.

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
