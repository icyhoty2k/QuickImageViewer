# qIV Remote Control over TCP/IP — Specification

Status: **agreed design, not yet implemented**

Everything remote-control lives in `src/Rem_TCP_IP/`. This document is the record of
what was decided and why, so the reasoning is not lost between sessions.

---

## 1. Purpose

Drive a running qIV instance over the network — from another machine, a phone, a
script, a home-automation system, or from another qIV instance.

The primary use case is the one Dedicated mode already exists for: screens dropped
on a machine, pointed at a monitor and forgotten. Those need to be driven without
someone standing at the keyboard.

### Why TCP and not `WM_COPYDATA`

An earlier design extended the existing `WM_COPYDATA` channel (`AppMain.cpp`, the
`dwData == 1` file-open path). It was rejected for good reasons:

- **`WM_COPYDATA` is same-session only.** It cannot reach another machine, which is
  the entire point of remote control for a wall of screens.
- **The sender would have to be `qiv.exe` itself.** In `WinMain`, `registryThread`,
  `wicThread` and `historyThread` all start *before* the single-instance mutex check.
  So `qiv.exe -remote next` would pay a full cold start — process spawn, registry
  read, WIC COM init, history file I/O — then discover an instance already exists,
  send a few bytes, detach three threads mid-flight and exit. Absurd for "next image".
- **No return channel.** `SendMessage` yields an `LRESULT` int and nothing more.

With TCP the sender is `curl`, netcat, Python, a phone, Home Assistant — anything.
No process spawn, cross-machine, and reading state back is possible later.

---

## 2. Placement

```
src/Rem_TCP_IP/
  RemoteServer.h/.cpp     listener thread, accept loop, access gates
  RemoteClient.h/.cpp     "connect to another instance" side
  RemoteWnd.h/.cpp        the panel
  RemoteSettings.h/.cpp   [REMOTE_TCP_IP] persistence
  RemoteProtocol.h/.cpp   wire format + command-name table
```

The panel lives here too, **not** in `src/UI/FloatingPanels/`. This mirrors
`src/Dedicated/`, which already keeps its own window, settings and lists together as
a self-contained subsystem.

### Build wiring

Two edits to `CMakeLists.txt`:

1. each `.cpp` added to `SOURCES`
2. `${CMAKE_CURRENT_SOURCE_DIR}/src/Rem_TCP_IP` added to `target_include_directories`,
   so the `#include "Rem_TCP_IP/RemoteServer.h"` sub-path convention works like every
   other folder

`ws2_32` is **already linked** (section 6 of `CMakeLists.txt`, tagged
`# Required by resvg`). Winsock needs no new dependency.

---

## 3. Activation

**The server is off by default and cannot be started by accident.**

Two ways in, and only two:

1. command-line arguments
2. a `[REMOTE_TCP_IP]` section in the instance `.ini`

```ini
[REMOTE_TCP_IP]
Enable=false
Name=
IpAddress=127.0.0.1
PortNo=
AllowList=
Password=
BlackList=
MaxConnections=1
```

| Field | Meaning |
|---|---|
| `Enable` | master switch. `true`/`false` (`ParseFlag` semantics — `1/true/on/yes`) |
| `Name` | identifies this instance to clients |
| `IpAddress` | bind address. `127.0.0.1` = this machine only; `0.0.0.0` = every interface |
| `PortNo` | listen port |
| `AllowList` | IPs permitted to connect. **Empty = deny everyone** |
| `Password` | stored hashed, never plaintext (see §5) |
| `BlackList` | IPs always refused. Beats `AllowList` |
| `MaxConnections` | simultaneous clients. Clamped to `[MIN, MAX]` |

### Bind address

`0.0.0.0` means `INADDR_ANY` — listen on *every* network interface, including ones
added later. `127.0.0.1` keeps traffic on the loopback: reachable only from this
machine, and Windows Firewall never prompts.

Note the panel has both a **bind** field (server) and a **connect-to** field
(client). `0.0.0.0` is meaningful only in the first. Label them distinctly.

### Constants

No magic numbers — all tunables named, per house rule:

```cpp
namespace Constants::RemoteTcpIp {
    constexpr int MAX_CONNECTIONS_MIN     = 1;
    constexpr int MAX_CONNECTIONS_MAX     = 99;
    constexpr int MAX_CONNECTIONS_DEFAULT = 1;
}
```

Values outside the range, or unparseable, fall back to `DEFAULT` rather than
refusing to start.

---

## 4. Access control

### Accept order

Checks run in this order on every accepted socket. The order is deliberate:

1. **BlackList hit** → close immediately, **no reply**
2. **Not in AllowList** (or AllowList empty) → close immediately, **no reply**
3. **At MaxConnections** → send a reason line, then close
4. **Password** → challenge-response
5. serve

Only a client that has already passed the address gates is told *why* it was
refused. A blocked IP learns nothing — not even that something is listening.

### Empty AllowList denies everyone

Fail-closed, consistent with `Enable=false` being the default.

Consequence worth handling in the UI: `Enable=true` with an empty `AllowList` is a
server that refuses everything, which looks like a bug from outside. The panel
status line must say so explicitly — *"Listening — AllowList empty, all connections
denied"* — not merely *"Running"*.

---

## 5. Password handling

Stored **hashed** in the `.ini`, never in plaintext. The stored form is
`<salt-hex>$<digest-hex>`, where digest = `SHA-256(salt + utf8(password))` and the
salt is fresh per password.

Authentication is **challenge-response**:

```
server → AUTH <salt-hex> <nonce-hex>
client → AUTH <hmac-hex>            where hmac = HMAC-SHA256(digest, nonce)
```

The password itself never crosses the wire, and a captured response cannot be
replayed against a different nonce.

**The salt must travel, and this was not obvious.** The two ends reach the same
shared secret by opposite routes: the server has the stored `salt$digest` and no
plaintext; the client has the plaintext and no stored value. Without the salt in
the challenge, a client holding the *correct* password still could not derive the
digest, and no exchange would be possible at all. Sending it costs nothing — a
salt's job is uniqueness, not secrecy.

`Crypto::SecretFromPassword` is the single definition of that arithmetic;
`HashPassword`, `VerifyPassword` and the client all route through it, so the two
ends cannot drift apart on how the digest is formed.

This fixes both exposures a naive design would have — plaintext at rest in a text
file anyone can read, and plaintext on the wire visible to anything sniffing the LAN
— without requiring TLS.

---

## 6. Kiosk lock

**Remote commands are obeyed even when `app.isLocked` is set.**

Coherent rather than contradictory: `isLocked` exists to stop a passer-by at the
keyboard of a wall-mounted screen. Remote access is already gated behind AllowList
and password. Different threat model, different gate.

---

## 7. Protocol

### Commands reuse `enum class Command`

No second vocabulary to maintain. Received commands land in
`InputManager::ExecuteCommand` — the same shared sink keyboard, mouse and tray
already funnel through, so remote behaviour is identical to local by construction.

**Including the payload commands**, which is worth stating because it was not
always true. The server used to call `Remote::ExecutePayload` directly to obtain
the reply line the sink's `void` overload threw away, so those commands reached
the handler without passing the sink — and everything hanging off the sink, the
crash breadcrumb and the observer echo, silently did not cover them. The overload
returns the reply now, so there is nothing left to route around, and
`ExecutePayload` has exactly one caller: inside the sink.

### Names on the wire, never ordinals

**This is a hard constraint, not a preference.**

`Command` enum ordinals renumber whenever anyone inserts an enumerator.
`ToggleViewportLock` was inserted mid-enum during the same session this spec was
written, shifting every command after it. A script sending ordinal `47` would
silently begin doing something else after a rebuild, with no error anywhere.

Names cost one lookup table in `RemoteProtocol.h`. That table doubles as the list of
what is remotely reachable — adding a command to the wire API becomes a deliberate
act rather than an accident.

### Payload

```cpp
struct RemoteRequest {
    Command      cmd;
    std::wstring payload;   // empty for argument-less commands
};
```

Most commands take no payload and go straight to `ExecuteCommand`. Twenty rows
are marked `PayloadRule::Required`, in four groups:

| Command | Payload |
|---|---|
| `JumpToImage` | index |
| `OpenFile` | path |
| `SlideshowSetInterval` | milliseconds |
| `ZoomTo` | percent |
| `FindImage` | search string |
| `Observe` | `1` / `0` |
| `Sync` | `k=v;k=v;…` — the whole view/effect state |
| `EnableRemoteLog` | `1` / `0` |
| `msgRemote` | text to show on that screen |
| `ShowImageOnce` | path |
| `StreamImageBegin` | `<totalBytes> <fileName>` |
| `StreamImageChunk` | base64 slice |
| `SendDisplayedPreview` | max dimension |
| `ImageChanged` | `<n>/<total> <file name>` |
| `FolderChanged` | `<reason> <path>` |
| `TogglesChanged` | `<Name>=<value>` |

**The headless paths exist.** An earlier version of this section said these
"currently raise UI — `JumpToImage` opens a panel, `SlideshowSetInterval` prompts
a dialog" and listed that as remaining work. It was done: `Remote::ExecutePayload`
(`RemoteExec.cpp`) is the headless body, and the payload overload of
`InputManager::ExecuteCommand` runs it *before* the bare switch, so a payload
command never reaches the case that would open a window and hold the caller's
socket until somebody dismissed it.

### The three announcements

`ImageChanged`, `FolderChanged` and `TogglesChanged` are the odd rows above: they
are not instructions. An observed instance PUSHES them, prefixed `EVENT`, to say
what it is now showing or what changed. **Executing one does nothing** — the
handler acknowledges the payload and returns — which is what makes them safe to
send to a peer that replays what it receives.

They exist because an observer is otherwise told only about COMMANDS, and three
things a client displays never arrive that way: a picture changed by the
slideshow timer (no command runs at all), a folder that turned out to hold
nothing, and a value moved by a command that is never echoed — a panel opened at
the keyboard, or one command clearing several toggles at once. See
`REMOTE_MIRRORING.md` §3.

---

## 8. Threading

The socket thread **never touches `app.*`, `app.playlist` or any GDI handle.** This
is the existing house rule and it is absolute.

Received commands are marshalled to the UI thread with `PostMessage` using new
`WM_QIV_REMOTE_*` messages, exactly as the decoder and scan workers already do.

---

## 9. The panel

Own window in `src/Rem_TCP_IP/`, shaped like the F8 Dedicated panel — a form, not a
menu.

**Fields:** Name, bind IP, port, AllowList, BlackList, password, MaxConnections
**Buttons:** Start · Stop · Check Connection · Connect to Another Instance

The panel both **configures and generates/regenerates** the `.ini`.

### Client side

"Connect to Another Instance" connects to an already-running peer server, using the
target IP / port / password entered in the panel. One-off commands — **not** action
mirroring or wall-sync. qIV is therefore both server and client.

---

## 10. `.ini` generation

### Per-key writes, never a wholesale rewrite

The file also holds `[Instance]` and `[Settings]` — real configuration for a
dedicated screen. `WritePrivateProfileStringW` writes one key and leaves the rest
intact, so regeneration is non-destructive by construction. A full rewrite would
silently eat the other sections.

The file is UTF-16LE **with a BOM**, deliberately: the Win32 profile API only writes
Unicode into a file that is already Unicode. When creating fresh, the BOM goes first.

### Generation seeds every current setting

Creating an `.ini` has a large hidden consequence. Per `Dedicated::DetectStartupMode()`,
an `.ini` beside the exe means `SettingsUseFile()` is true — *all* settings come from
the file and the registry is untouched from the next launch onward.

So on a registry-mode install, saving from the remote panel would flip the entire app
from registry-backed to file-backed, and every existing setting would fall back to
its default because the new file does not contain them.

**Resolution:** generation seeds `[Settings]` with every current value first, making
the flip lossless rather than merely disclosed.

Creation order when the file does not exist:

1. UTF-16LE BOM
2. `[Instance]` identity
3. `[Settings]` — every current setting
4. `[REMOTE_TCP_IP]`

Step 3 reuses the key list the tray **Export** already walks — same `Registry::*`
names and `app.*` values. Share the *enumeration*, not the writer: Export emits a
`[QuickImageViewer]` block to a `.txt`, while the `.ini` wants `[Settings]`.

Regenerating an existing `.ini` skips 1–3 and touches only the `[REMOTE_TCP_IP]` keys.

### A third section

`DedicatedSettings` currently knows exactly two sections — `[Instance]` and
`[Settings]` — with hardcoded accessors. `[REMOTE_TCP_IP]` is a third, so the
existing read/write helpers should be generalized to take a section name rather than
growing a parallel set of `ReadRemote*` functions.

---

## 11. Build order

Each slice independently testable. Sockets deliberately last.

1. **Constants + settings** — `[REMOTE_TCP_IP]` read/write, generalized section
   accessors, cmdArgs parsing. No sockets. *Test: config loads, clamps, defaults hold*
2. **Protocol** — name↔`Command` table, `RemoteRequest` parse. No sockets.
   *Test: strings resolve, junk rejected*
3. **Server** — listener thread, accept gates in order, password challenge, marshal to
   UI thread. *Test: drive it with netcat*
4. **Payload paths** — the five headless variants
5. **Client** — connect to peer, send
6. **Panel** — fields, buttons, `.ini` generate with seeding

---

## 12. Deferred

- **`-query current`** style state readback. TCP supports it; not in the initial scope.
- **IPv6.** Unspecified so far.
- **Latent bug, unrelated but adjacent:** `AppMain.cpp` constructs
  `new std::wstring((LPCWSTR)cds->lpData)` from `WM_COPYDATA` with **no `cbData`
  length check**. A sender passing a non-NUL-terminated buffer reads past the end.
  Local-only and low severity, but worth fixing.
