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

Three values, cycled by the panel:

| Value | Meaning |
|---|---|
| `127.0.0.1` | Loopback only — reachable from this machine, and Windows Firewall never prompts. The default, and what a new `AllowList` is seeded with |
| `0.0.0.0` | Every interface, **IPv4 only**. The socket family follows the literal |
| `::` | Every interface, **dual stack** — IPv6 *and* IPv4 on one socket |

**`::` is not a synonym for `0.0.0.0`.** A mobile client on a carrier that hands
out no IPv4 address can reach `::` and nothing else, which is the reason the third
value exists rather than being a tidier spelling of the second.

**Windows defaults `IPV6_V6ONLY` to 1**, so a listener on `::` would accept IPv6
and *refuse* IPv4 — on a machine whose LAN is v4 that looks like a server which
started cleanly and cannot be reached by anything. `Start()` therefore clears the
option when the resolved family is `AF_INET6`, so one socket serves both families
and v4 peers arrive as `::ffff:a.b.c.d`. Clearing it is **best effort**: a failure
leaves the documented v6-only behaviour, which is still a working listener, so it
must not refuse to start.

⚠ **IPv4-mapped addresses are normalised away before the access rules see them.**
`AcceptedPeerAddress` flattens `::ffff:192.168.1.5` back to `192.168.1.5`, so the
`AllowList` and `BlackList` keep exactly the shape they had before dual stack
existed. Nothing in the access path needs to know which family a peer arrived on.

**`SO_REUSEADDR` is deliberately NOT set.** On Windows it lets two processes bind
one port and silently steal each other's connections, which for a control channel
is a security problem rather than a convenience. A port already in use fails loudly.

Note the panel has both a **bind** field (server) and a **connect-to** field
(client). `0.0.0.0` and `::` are meaningful only in the first. Label them
distinctly.

### TCP keepalive — for NAT, not for a slow peer

Enabled on **both** ends: server-accepted sockets and client-connected ones, from
one helper in `RemoteProtocol.cpp`.

An authenticated connection deliberately has **no receive timeout** — a mirrored
screen is supposed to sit silent for minutes. On a LAN that is free. Across a home
router it is not: a NAT mapping with no traffic through it is discarded after a few
minutes, and **the discard is silent in both directions.** Neither end sees a
close, so the server thread stays blocked in `recv()` on a socket that can never
deliver again while the driving end's row stays green and the mirror does nothing.

🔥 **That failure has no other detection path**, because the whole design of an idle
mirror is that nothing is sent.

`SIO_KEEPALIVE_VALS` rather than `setsockopt(SO_KEEPALIVE)`: the plain option turns
keepalive on but leaves the **system-wide** defaults in force, which on Windows is
two hours — far too late to matter here.

| Constant | Value | Why |
|---|---|---|
| `KEEPALIVE_IDLE_MS` | 60000 | Consumer NAT idle timeouts start around five minutes; one minute is comfortably inside the shortest of them, at one empty segment a minute per connection |
| `KEEPALIVE_INTERVAL_MS` | 10000 | Gap between probes once one goes unanswered |

⚠ **The retry count is not settable.** Windows fixes it at 10 on Vista and later
and `SIO_KEEPALIVE_VALS` cannot change it, so the interval is the only lever on how
fast a dead peer is declared dead: 10 s × 10 ≈ 100 s after the idle period.

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

### Blocking an IPv6 peer means blocking its `/64`

`Remote::BlockScope(address)` decides what a ban, a timed kick or the brute-force
guard actually writes:

| Input | Written |
|---|---|
| IPv4 literal, or anything that does not parse | unchanged |
| IPv6 host address | the first four groups, then `::/64` |

🔥 **Banning one IPv6 address is close to useless — the peer has 2^64 of them.** A
single machine can step to the next address in its own prefix for every retry, so a
per-address rule is a rule it never notices. The `/64` is the smallest unit that
corresponds to *one site* rather than one interface.

Three inputs are returned untouched, and each is a correctness case rather than a
tidiness one:

- ⚠ **IPv4-mapped (`::ffff:192.168.1.5`) is not an IPv6 host.** Its low 64 bits
  carry a v4 address, so a `/64` over one would read `::ffff:0:0/64` and block **the
  entire IPv4 internet** from a single wrong password. `AcceptedPeerAddress` already
  flattens these before anything here sees them, so this is a second line of
  defence — but it is the one mistake in the function that would be catastrophic
  rather than merely wrong.
- **An all-zero prefix** is `::` and its neighbours, where loopback lives. Nothing
  routable arrives with one; this is a guard, not a case.
- **Anything that is not an address at all** — host-name entries have no prefix to
  widen.

The prefix is printed lower case with no leading zeros, so two spellings of one
prefix cannot produce two blacklist rows.

### Timed blocks — the middle option between a kick and a ban

`KickConnection` alone is answered by a reconnect a second later; a ban is a
permanent decision nobody wants to make about an address that may be a customer
tomorrow. **A timed block outlasts a retry loop and then forgets.**

```cpp
namespace Remote::Blacklist {
    void AddTimed(const std::wstring &address, int minutes, const std::wstring &reason);
    bool ClearTimed(const std::wstring &address);   // false when it had none
    std::vector<TimedEntry> TimedSnapshot();        // live entries, expired pruned
}
```

`Server::TimedKickConnection(id, minutes, scopeOut)` kicks and blocks in one step,
scoping through `BlockScope` exactly like `Ban`, and reports back what was really
blocked so the panel can tell the operator they blocked a prefix rather than an
address.

**In memory only, never written to `qivRemoteServerBlacklist.ini`, and a restart
clears every one.** That is deliberate: a timed block is a reaction to something
happening right now, and a file that outlived the incident would accumulate rules
nobody remembers making.

Two behaviours worth not re-deriving:

- **A second `AddTimed` on one address REPLACES the first**, and the newest expiry
  wins whether it is longer or shorter. Accumulating would make "blocked for ten
  minutes" mean something different depending on what had been pressed before.
- **Full (`TIMED_BLOCK_MAX`) drops the soonest-expiring entry**, rather than
  refusing like the permanent list does. The permanent list fails closed on a file
  it cannot grow; this is a live decision an operator just made and is entitled to
  see take effect, and the entry closest to expiring is the one whose loss changes
  least.

⚠ **`IsBlocked` asks both lists and must have one answer.** The timed table is
checked first, then the permanent one. Keeping them in the same module is the point:
a second list consulted from a second place is how an address ends up blocked by one
rule and admitted by another.

**Auto-blacklist entries still never expire** — the brute-force guard writes
permanent rows. Now that the timed machinery exists, an escalating first offence
would suit it; not done.

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

### My Clients — a second panel, <kbd>Ctrl+F9</kbd>

`RemoteClientsWnd` answers a question <kbd>F9</kbd> cannot: **who is connected to me
right now, and how do I get rid of them.** <kbd>F9</kbd> configures the listener;
this one operates it while it runs.

The list holds two kinds of row — live connections, and current timed blocks — so
the thing an operator just did is visible beside the thing it did it to.

| Button | Effect |
|---|---|
| **Kick** | `shutdown()` on that connection. **Not a ban** — the same peer may reconnect immediately |
| **Kick for…** | Kick, then refuse the peer for N minutes. `TimedKickConnection` |
| **Ban** | Blacklist, *then* kick, in that order so a reconnect racing the disconnect is refused at the accept gate rather than admitted |
| **Lift** | Clears a selected timed block early. Enabled only on a block row |

Both destructive buttons confirm first, and the result line reports **what was
actually blocked** — appending *"(the whole /64)"* when `BlockScope` widened an IPv6
address, because an operator who thinks they blocked one address should not discover
otherwise later.

⚠ **Kick uses `shutdown()`, never `closesocket()`.** The client thread owns that
socket and closes it itself; shutting it down makes its blocked `recv()` return so
the thread unwinds through its ordinary path — logging its departure, leaving the
observer list, releasing its locks. Closing the descriptor from here would race that
thread and could shut down a number the system had already reissued.

🔥 **Never hold a reference to a row across a dialog.** Every action here copies the
connection id and address *before* opening its confirmation, because the modal loop
pumps messages, the panel refreshes, and `m_rows` is rebuilt underneath. Taking a
reference and using it after the dialog returns crashed `DoKick`.

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

**Everything this section used to list has since shipped.** Kept as a record, because
a deferred list that is silently wrong is worse than no list — it invites the same
work to be planned twice.

- ✅ **State readback.** `QueryState`, `QueryToggles`, `QueryHistory` and
  `QueryClients` all exist and are read-only.
- ✅ **IPv6.** Dual stack, `::` in the bind cycle, `IPV6_V6ONLY` cleared, and
  `BlockScope` so a v6 ban means the `/64`. See §3 and §4.
- ✅ **The `WM_COPYDATA` length check.** `AppMain.cpp` now requires
  `cbData >= sizeof(wchar_t)` and scans with `wcsnlen(raw, cds->cbData / sizeof(wchar_t))`
  instead of trusting a NUL the sender was never obliged to write. Any process on
  the desktop may post that message, so `cbData` is the only thing bounding the
  buffer.

Still open, and small:

- **A permanent auto-blacklist entry never expires.** It ages out of nothing, and
  clearing one means editing `qivRemoteServerBlacklist.ini` by hand.

  **The first offence is no longer permanent, as of v3.** Five failures inside the
  window now cost a fifteen-minute timed block, and only a repeat writes the file -
  see `Remote::AuthPolicy`, which holds that decision as pure, tested logic. What
  remains open is ageing out the permanent rows, which is a separate question: the
  file is the record of who is barred, and entries that vanish on their own make it
  answer that question differently over time.

Closed since this list was written:

- **`ExifWnd`'s scrollbar drag never called `SetCapture`.** Swept in v3: scroll-drag
  capture is centralised in `FloatingPanelWnd`, with `SetCapture` on the thumb press
  and a release on `WM_CAPTURECHANGED` however the drag ends, and every panel in the
  program derives from it. `InputBox` handles its own text-selection drag without
  capture on purpose - it self-terminates on the `MK_LBUTTON` bit and says so. **No
  second instance of the shape exists.**
