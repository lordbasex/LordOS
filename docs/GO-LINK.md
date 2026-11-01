# go-link Desk on LordOS

The design of **go-link Desk for LordOS** and of the public **screen-sharing API** it needs. go-link Desk (desk.go-link.org) is the
maintainer's own remote desktop project, MIT, in a separate repository. On LordOS it is an optional third-party app in the Store:
recommended, not privileged, not part of LordOS. The person installs it or doesn't.
**Nothing here is built, and nothing here has been tested.** This is the design.

How to read it:

- **Read** means checked in the code or documents of both repositories on 2026-11-01, with the place given. It says what the code
  says, not that anyone ran it. **Measured** means a number already published in LordOS's documents, on the machine named. **To test**
  means nobody has run it. Any number under "target" or "estimate" is a goal or a guess, not a result.
- Paths without a prefix are LordOS's, on branch `desk-go-link`. Paths starting `go-link:` are in the go-link Desk repository, under
  `backend-device/` unless they start with `docs/`.

## 1. Decisions already taken

These are the maintainer's, final, and this document builds on them.

| Subject | Decision |
|---|---|
| Two solutions | go-link does not replace remote desktop on port 5673. 5673 is inbound, on LAN or VPN, with machine credentials. go-link is outbound, no open ports, a code plus acceptance on screen, through signalhub and TURN. |
| One or the other | Per session, one door at a time. The system enforces it at the API (section 5). Neither ever kicks the other out. |
| Separate door | The API is not 5673's path exposed. It reuses only the composed frame, the `mstream` encoder, Opus and the input message formats. |
| Interface | A native LordOS app in React and JavaScript, drawn by our compositor. No Fyne, X11, Xwayland, portal or PipeWire capture. |
| Everything else | Stays Go: signaling, WebRTC (Pion), control, connections. Same functions as the Linux version, its own privacy policy. |
| Engine | Split from the Fyne UI: a headless engine talking to the app over a local socket, including the acceptance request. |
| API | Public and general, not go-link-specific: permission prompt on first use, always-visible indicator with a stop control, encoded frames plus Opus, input, protected windows black. |
| Two consents | The system permission (this app may use the API) and the acceptance of each visitor (this person may come in) are separate things. |
| Delivery | `.deb` for amd64 and arm64 published first at desk.go-link.org with a signature, verified and mirrored byte for byte to the LordOS APT repository, a Store item, updates through APT, the app's own updater off, a published version never re-uploaded. |
| Schedule | The API ships in **LordOS 0.8.7.1**, a package update through the Store and APT with no new ISO, before Machines in 0.8.8. The app requires 0.8.7.1 or later. |

Non-goals: replacing 5673, viewing a LordOS machine from a native LordOS viewer (the Linux version's Connect opens the website,
go-link:`docs/architecture.md:273`, and so does this one), and the video call, which the Linux version doesn't have either (go-link:`pkg/camera/camera_other.go:8`).

## 2. What the code says today

Five findings shaped the rest. All **Read**.

1. **Nothing lets a third-party native app run a helper.** Section 3 has the details.
2. **`remote.sock` has no authorization inside the user.** `mirror_dispatch` accepts root or the same uid and nothing else
   (`server/src/remote/mirror.c:554`). Any unsandboxed process of the user can send `RP_VIDEO` and get the encoded screen, or send
   `RP_PTR` and `RP_KEY`, with no prompt. A client that never sends `RP_HELLO` isn't listed in the on-screen notice
   (`mirror.c:128`). This is why the API must be a separate door, and it's also what makes the proof of concept possible (section 11).
3. **A locked session keeps existing.** Locking takes the monitor from the session; the session goes on drawing in memory for anyone
   watching (`docs/SESSIONS.md:40`, `:83`). 5673 is safe because its bridge cuts itself on the lock message
   (`server/src/remote/conn.c:750-757`), not because frames stop. The API must end on the lock in `lord-server` itself.
4. **Two 5673 habits are wrong for a third party.** A new client gets the clipboard as it is at connection (`mirror.c:207`), and
   go-link's rule is that only what is copied afterwards leaves (go-link:`docs/security.md:43`). And `mirror_clipboard` doesn't look at
   the sensitive mark Passwords sets (`mirror.c:762`, `server/src/term/term.c:202`). The API applies both rules; 5673 stays as it is.
5. **The engine can be pure Go here.** The encoders are the only reason the Linux host needs cgo for the picture, and on LordOS frames
   arrive encoded. go-link already vets a `CGO_ENABLED=0 -tags headless` build (go-link:`Makefile:93`) and has the no-cgo stubs
   (go-link:`pkg/encoder/nocgo.go:3`). So no libvpx, libopus, X11 or wayland-client, which also makes the sandbox simple. To test.

## 3. Delivery and sandbox of a third-party app with a helper

### 3.1 What the rules allow today

| Route | Result |
|---|---|
| System app in `assets/sysapps/<id>/` | Refused by design: every file LordOS runs as JavaScript is in a manifest signed with the LordOS release key (`docs/SIGNING.md:167-176`). It would also make go-link privileged. |
| `.lpk` in the home folder | The JS works, with no helper: files are written 0644 (`server/src/lpk.c:219`), JS can't run commands (`docs/PAPER.md:33`), and the seccomp filter refuses `execve` and `AF_UNIX` sockets (`docs/PRIVACY.md:185-190`, `server/src/sec/sandbox.c:561-568`). A user-writable binary shouldn't run anyway. |
| `.deb` with a `.desktop` file | The engine runs through the launcher in `app-lordos-<id>.scope`, unsandboxed by default (`docs/PRIVACY.md:40`). No native UI, and it couldn't talk to an isolated app. |
| `.deb` that adds a native app | Not possible: `app_package_dir` looks only in `assets/sysapps` and the home root (`server/src/app/appspawn.c:27-40`). |
| `lord-rdesk` model | The right shape, but for LordOS's own binary, found next to `lord-server` (`server/src/ui/js_rdesk.c:39-47`). `sandbox_apply_helper` also forbids all sockets (`docs/PRIVACY.md:283`), which an engine can't live with. |

**Answer: the rules don't allow it today.** The changes below are the smallest safe set found. They add a fourth kind of thing to
LordOS, so this is **open decision D1**, and the new command exception must be approved and listed in `AGENTS.md` and
`docs/PAPER.md:33`.

### 3.2 The proposed change: a packaged app with a companion

1. **A third root for apps, read-only.** `/usr/lib/lordos-apps/<id>/`, installed by a `.deb`, with the same layout as an unpacked
   `.lpk`. `app_package_dir` accepts it only if owned by root and not writable by group or others, the stricter form of the sysapps
   check. It is never in `code-manifest.json`, never `system: true`, and runs as `lord-server --app <id>` in the usual sandbox.
2. **Checked at every load as a package, not as LordOS.** `contenido.json` and `firma.json` are verified with the existing code
   (`server/src/sec/sign.c`). A packaged app loads only if its key is a **Verified author** in LordOS's signed catalog
   (`docs/SIGNING.md:64`, `:77`). So LordOS says "this key may ship a packaged app", and go-link signs its own releases.
   An app update needs no LordOS release. The key can be revoked with the catalog's list (`catalog_revoked`).
3. **The manifest may name one companion:** `"companion": "engine"`, honored only in that root. The binary is listed in `contenido.json`,
   so go-link's signature covers it. A `.lpk` in the home folder saying the same is ignored.
4. **One new broker op, `BOP_COMPANION`.** The app asks; `lord-server` takes the id from the verified connection, never from the
   request. It accepts no path and no arguments. In the forked child, never in the compositor's thread, the binary is opened, hashed against
   the signed list, and run from that same descriptor with `execveat(AT_EMPTY_PATH)`, so there's no window between the check and the use.
   The hash runs in the child because it could take a large fraction of a second on a slow disk. To test.
5. **It runs in the app's own scope,** `app-lordos-lpk-<id>.scope`. The child places itself before exec, which the placement service
   accepts because it is still `lord-server` (`docs/PRIVACY.md:697-704`). Monitor and the firewall see one app, `lpk:<id>`. The scope's
   limits are one core and 512 MB (`docs/PRIVACY.md:199`): whether the UI and the engine fit is **To test**, and raising them for a
   packaged app is a one-line change if not.
6. **Two inherited descriptors,** and no others. Fd 3 goes to the app (the broker passes the other end with `SCM_RIGHTS`, as the
   system file dialog does). Fd 4 is the share connection, already tied to this app id. The engine dies with the app: end of file on
   fd 3 means exit.

### 3.3 The engine's sandbox

The sandbox is applied before exec, because Go can't apply ours from inside; `sandbox_exec_child` already works this way
(`server/src/sec/sandbox.c:639`). A new `sandbox_exec_companion`:

| Limit | Value |
|---|---|
| Identity | no capabilities, `no_new_privs`, the app's scope, dies with the app |
| Landlock, read | the app's folder, `/etc/ssl`, `/etc/resolv.conf`, `/run/systemd/resolve` |
| Landlock, write | `~/.local/share/lordos/appdata/<id>` only: `device.json` and the logs live here |
| Landlock, execute | only the engine, so an `execve` left in the filter can start nothing else |
| seccomp | a new allowlist for a static Go program; `AF_INET` and `AF_INET6` only with the `network` permission, `AF_NETLINK` route for listing interfaces, **never `AF_UNIX`** |

No `AF_UNIX` is the point: Landlock doesn't filter `connect` to named sockets (`docs/PRIVACY.md:189`), so with it the engine could
reach `remote.sock` and the session bus. On a kernel without Landlock the app is shown as weaker, as Machines does; seccomp must still
apply or the companion doesn't start. Pion's exact syscalls and its use of netlink are **To test**.

**Files.** go-link's file service browses the home folder and writes to Downloads (go-link:`cmd/desk/main.go:394`). A sandboxed
engine can't do that, and go-link's own filter for `.ssh` is code inside the process we're containing. Decision: the first version
gets a `Shared with viewers` folder, `~/Downloads/go-link`, opened by `lord-server` and passed as a descriptor. This is less than the Linux
version's function, so it is **open decision D2**.

## 4. The screen-sharing API

### 4.1 Shape

A new file, `server/src/remote/share.c`, in `lord-server`, with its own socket, client table and parser. It doesn't reuse `Client` or
`handle()` in `mirror.c`. Framing is the same as `proto.h` (u32 length, u8 type) and the media and input payloads keep their formats,
so a client sees `RP_VFRAME`, `RP_AFRAME`, `RP_PTR`, `RP_BTN`, `RP_WHEEL`, `RP_KEY` and `RP_SETCLIP` as documented there.

| Who connects | How |
|---|---|
| A companion | fd 4, connected by `lord-server` at spawn, id known (`lpk:<id>`) |
| Any Linux app started by the launcher | `$XDG_RUNTIME_DIR/lordos/share.sock`, 0600. The id comes from the peer's root-owned scope (`appid_of_pid`, `server/src/sec/appid.h`), read through `SO_PEERPIDFD` so a dead pid can't be reused |
| Anything else | Refused with `no_identity`: Terminal programs, `systemd-run --user`, processes in no app scope |
| An isolated `.lpk` with no companion | Not in the first version: it has no way to use a socket. A broker route can come later |

The listener isn't created for the login screen's `lord-server`.

**Client to system:** `SHARE_START {video, audio, input, clipboard}`, `RP_KEYFRAME`, `RP_RATE`, `RP_RESIZE`, `SHARE_AWAKE {on}`, `SHARE_LOCK`,
`SHARE_STOP`, the input messages, and one new one, `SHARE_TEXT` (UTF-8, at most 1 KB), because go-link's text packet
(go-link:`docs/protocol.md:185`) has no equivalent today.

**System to client:** `SHARE_STATE {granted, codecs, w, h, hz}`, `SHARE_REFUSED {reason}`, `SHARE_END {reason}`, `RP_SIZE`,
`RP_VFRAME`, `RP_AFRAME`, `RP_CURSOR`, `RP_SHAPE`, `RP_CLIP`, `RP_VINFO`.

`proto.h:57` documents `RP_VIDEO`'s third field as `u16 0`, but it carries the refresh rate (`mirror.c:304-306`). Fix the comment
when this is written.

### 4.2 Authorization, storage and revocation

Two new permissions in `perms.c`: `screen-share` (picture and sound) and `remote-input` (mouse, keyboard, clipboard). Both must be declared
in the manifest, or by a Linux app at its first `SHARE_START`. Existing `screen` stays for screenshots and recording.

- **The prompt** is system-drawn, in the `system` layer, on first use: `"go-link Desk" wants to share your screen with people you
  accept`, then the same for input. For a `desktop:` id it adds the program's path and "This app isn't verified".
- **The store is root-owned:** `/var/lib/lordos/share/<uid>.json`. `permisos.json` is protected from isolated apps
  (`docs/PRIVACY.md:360`) but an unsandboxed Linux app of the same user can edit it, and this permission is the one worth stealing.
  A new closed action `share-grant <id> <perm> allow|deny|ask`, marked `A_ANYONE` (`server/src/admin.c:370`), acts on the caller's own
  uid only and validates the id. `lord-server` reads the file; nothing else writes it.
- **A Linux app's grant is tied to its id and executable path.** A changed path asks again.
- **Revoking:** Settings → Privacy lists both permissions. Revoking ends a running share at once, in the same way the camera does
  (`docs/PRIVACY.md:367`). Uninstalling forgets it.
- **Firewall restrictions win:** the `FW_R_NO_SCREEN` flag (`server/src/sec/restrict.c:29`) refuses without a prompt, as it does for capture.
- **Grant without a person:** if no one can answer the prompt, the answer is `no_person`, never a default.

### 4.3 The indicator

- A new `ShareBadge`, mounted by `startShell` in every theme like `RemoteBadge` (`sdk/remote.js:4`), with the app's name and **Stop**, on
  every display, in the `notification` layer, above fullscreen windows ([LAYERS.md](LAYERS.md)). Unlike the remote desktop badge, it
  takes clicks.
- **It fails closed.** `share.c` sends no frame until the shell reports the badge is on screen. If the shell doesn't answer in 2 s
  (target), the share is refused with `no_indicator`.
- **A stop that needs no JavaScript.** A shortcut handled in C ends the share even if the shell hangs, and injected input can't press it.
  The key is chosen in [SHORTCUTS.md](SHORTCUTS.md)'s list when this is written.
- Monitor → Network lists the holder. Settings → Sharing shows "In use by go-link Desk" while it holds the session.

### 4.4 Viewers, size and rate

- **One holder per session.** A second app gets `busy`. How many people watch through that holder is the holder's business, and they all
  get the same stream. go-link allows one viewer at a time (go-link:`docs/architecture.md:64`).
- **Size:** the session's, without scaling. `mstream` has none, and a resize starts a new encoder (`server/src/media/mstream.c:255`).
  go-link's size steps on low bandwidth therefore don't apply on LordOS, and only the bit rate moves. `RP_RESIZE` works only on a session
  with no monitor, within 640×480 to 3840×2400 (`server/src/main.c:640`); the monitor wins, as for 5673.
- **Rate:** `RP_VIDEO`'s rate, 24 to 240 Hz as in `mstream`, and 300 kbit/s to 20 Mbit/s (`mstream.c:180`). A new rate restarts the encoder
  (`mstream.c:176`), so the client should keep it stable.
- **Keyframes:** on `RP_KEYFRAME`, at most one every 500 ms (target), because the encoder only makes them when asked (gop of 60 s,
  `server/src/media/helper/lm_stream.c:91`).
- **A slow client** falls behind the same 3 MB limit as a 5673 viewer: it skips to the next keyframe (`mirror.c:38`, `:419`), and
  `lord-server` never waits for it.

### 4.5 Encoder start and stop

`share.c` drives the same `mstream` code as the bridge does, one at a time. The first subscription calls `mstream_want` and `SHARE_END`
stops it. Because the session's frame loop asks `mirror_watchers` and `mirror_fps` whether anyone watches
(`server/src/main.c:656`, `:1958`, `:2384`), those two count the holder too. `mirror_frame`'s early return (`mirror.c:603`) goes
through the same answer. The `MStreamOut` callbacks send to whichever door holds the session. With no holder, these paths are as they
are today.

Sound: `SHARE_START` with `audio` opens the ring at the start, and `share.c` drops `RP_AFRAME` until the viewer may hear it, because sound
joining later restarts the encoder (`mstream.c:187`). It's mono Opus at 64 kbit/s (`docs/REMOTE.md:630`), while go-link offers stereo
at 96 kbit/s (go-link:`docs/protocol.md:155`). Mono in a stereo-negotiated track should play. To test.

### 4.6 Session states

| State | Starts? | While running |
|---|---|---|
| On the monitor, unlocked | yes | normal |
| No monitor present, session drawn in memory | with a stored grant; first use gives `no_person` | composes at 60 Hz or more (`main.c:1958`). The indicator is in the picture, not seen by anyone at the machine, so only unattended links can get here (D3) |
| Locked | no, `locked` | `SHARE_END {locked}`, synchronously in `mirror_session` (`mirror.c:515`), before another frame goes. No viewer can unlock: the login screen is another process |
| Switched to another user | no, `away` | `SHARE_END {away}`. The person who accepted has left |
| Login screen | never | a separate `lord-server` as `lordos-greeter`, no listener. It can't be shared or driven |
| Logged out | no | the socket closes |

Protected windows stay black because the API takes the frame after `media_composed` (`main.c:1500`) or the GPU's read-back. With a
protected window on screen, that means the CPU path (`main.c:655-658`). On the "main display" setting, viewers get the main display
(`docs/DISPLAYS.md:198`); two displays with a browser were never tested (`docs/DISPLAYS.md:253`).

### 4.7 Input

- `lord-server` injects through the same functions as 5673 (`main.c:516-561`), with values validated by `share.c`'s own parser and with
  rate limits (targets: 1,000 pointer moves and 200 keys a second).
- **System surfaces take no API input.** While anything shows in the `overlay`, `modal` or `system` layers, the compositor already knows,
  because it drops keyboard focus (`docs/PRIVACY.md:459-464`). API input is dropped then: a visitor can't press Allow on a prompt, or
  type into the keyring.
- **Protected windows take none either:** a key or a click aimed at one is dropped (`node_protected`, `server/src/wl/wl.c:2101`). A
  visitor shouldn't be able to drive what they can't see.
- **Buttons:** `RP_BTN` has three (`mirror.c:247`), so go-link's back and forward do nothing. Fine scrolling comes as `RP_SCROLL`, since
  `RP_WHEEL` is vertical only.
- **Held keys:** on `SHARE_END`, `share.c` releases everything the client pressed.
- **Clipboard:** only with `clipboard` in `SHARE_START`, text only, starting from what is copied after that. Items marked sensitive never go
  (`term_clipboard_sensitive`).

## 5. One door at a time

Enforced per session, in `lord-server`, by the single-threaded main loop, so whichever request arrives first wins. Another user's session
is independent: one user can be on 5673 while another shares through go-link.

**What counts:** a 5673 viewer is a `remote.sock` client with `RP_HELLO` from root, not the seat (a local process can't block sharing).
A share is active from `SHARE_START` accepted until `SHARE_END`.

| Situation | What happens | Reason shown |
|---|---|---|
| 5673 is on and listening, nobody connected | go-link shares normally. 5673 stays enabled. | none |
| A 5673 viewer connected, go-link's share starts | `SHARE_REFUSED {remote_desktop}`. The 5673 viewer sees nothing. | App: "This computer is being used through Remote Desktop." Visitor: the new reason below |
| Share active, a browser signs in on 5673 | A new `RP_BUSY {app}` to the bridge on `RP_HELLO`. The bridge sends `{"t":"bye","busy":…}`. The share sees nothing. | Page: "go-link Desk is sharing this session. Try again when it ends." |
| Either one ends | The other door is free at once | none |

- **Neither kicks the other out.** There is no code path from one door to the other's connection.
- **The 5673 sign-in stays valid** when it's refused, so no new password is needed. The page keeps its normal retry (`docs/REMOTE.md:353`, every
  30 s at most), so it comes in by itself when the session is free, and its text is added in English, Spanish and Portuguese.
- **A short network drop on 5673:** once its WebSocket is gone the viewer isn't connected, and a share may start. That's a real edge of
  the rule, and it's accepted: the page says why it can't come back.
- **go-link asks before bothering the person.** On a knock the engine asks `SHARE_STATE` first; if the session is taken, the visitor is turned
  down at once. `SHARE_START` still decides, for the race during the 60 seconds of the question.
- **A new go-link reason,** `rejected {reason: "in_use"}`, added to the website. Older viewers show a generic refusal. To test.
- **Who sees what:** with 5673 connected, the monitor shows the remote desktop badge as today. With a share, `ShareBadge`. Never both.
- **Other than this refusal, 5673 doesn't change.** Regression check in stage 1.

## 6. Encoding is not free

Capture is, in the sense the frame is already composed and one encoder serves the session. But the encoder costs CPU whenever a share
runs, whichever door uses it.

- x264 took **12–13 ms per frame** on VM 116 (**Measured**, 1280–1568 px wide, `docs/REMOTE.md:512`), a ceiling of about 75–80 fps.
- **The Raspberry Pi 5 has no H.264 hardware encoder:** x264 there too, with no measurement (`docs/REMOTE.md:527-528`).
- **VA-API is unmeasured** (`docs/REMOTE.md:492`, `:527`).
- **VP8** cost is unmeasured on LordOS. The bridge's dragging test had the encoder at 26–28 % of a core on VM 122 (**Measured**, H.264,
  `docs/REMOTE.md:471`).
- **"Capture is free" has limits.** On the GPU path each frame is read back, and a fullscreen app is composed instead of scanned out
  (`docs/REMOTE.md:280`). With a protected window, composition moves to the CPU. A session with no monitor starts drawing at all
  (`docs/SESSIONS.md:40`). Those costs aren't measured for this case.
- The picture is H.264 Constrained Baseline or VP8. A viewer that asks for High gets Baseline (section 8).

No claim on a screen until stage 5 has numbers per machine.

## 7. The app

The app is called **Desk go-link**. It has nothing to do with Remote Desk, LordOS's RDP and VNC client: separate app, separate icon, separate
code. It's a complete native copy of go-link's window: every view of the Fyne interface (`backend-device/internal/gui`, about 6,500 lines of
Go) rebuilt in React with `@lordos/sdk`, keeping go-link's look, layout and texts rather than LordOS's: the bar with the 9-digit code,
Session selector, Connect and the demo; the tabs; this computer's card with its code, link, copy, new code and status; My devices, Favorites
and Recent sessions with their thumbnails and views; Settings; the connection request with its scam warning; the session panel, chat,
calls, files, remote panels, network test and About. Texts come from go-link's catalogs.

The acceptance is drawn by the app, not the system: it's the visitor's consent, and it belongs to go-link. The system's part is the
permission and the indicator.

| Function of the Linux version | On LordOS |
|---|---|
| Code, new code, acceptance, permissions live, session panel, chat | same |
| Picture, sound, control, clipboard | through the API |
| Unattended access, linked browsers, password | same code; see D3 |
| Files | narrower, D2 |
| Keep the display awake | `SHARE_AWAKE`; `pkg/awake`'s D-Bus services aren't ours, and only while a viewer is accepted |
| Lock when a session ends | `SHARE_LOCK`, because `loginctl` can't work here: LordOS sessions have no logind seat (`docs/SESSIONS.md:465`, go-link:`internal/services/session_end.go:98`) |
| Open at login | not available to this app today, D4 |
| Icon in the top bar, by the clock | same menu as on the other platforms: the code, Disconnect, Open, Settings, Check for updates (opens the Store), Test my connection, About, Quit; red while a session lasts. Third-party top-bar icons need an API of their own: to design |
| Visible as an app | in Applications and the Dock, with go-link's icon |
| Update | off, by APT |
| Connect to another computer | native, in the same window, as in go-link: the viewer's tabs and panels, with the picture shown through the engine's MediaView path. Not designed yet |
| MCP for assistants, terminal | their views are copied too; what they run are other programs, which the sandbox's write rule doesn't allow today. Open: D9 |

## 8. Changes in go-link, by package

Other platforms are untouched: everything new is under a `lordos` build tag, and existing files change only in their build lines. The
build is `CGO_ENABLED=0 -tags headless,lordos`. Today's tags make that a small step: `headless` already drops X11 and Fyne
(go-link:`pkg/capture/capture_linux.go:3`) and picks the stubs (go-link:`pkg/capture/capture_other.go:3`).

| Package | Change |
|---|---|
| `pkg/lordshare` (new) | The client of section 4: framing, messages, input and output queues. No go-link types, so it can be tested alone. |
| `pkg/capture` | `capture_lordos.go` with `Displays` (one) and the `Supported` answers; `capture_other.go` gets `&& !lordos`. The `Frame` type is a raw picture, so frames don't pass here. |
| `pkg/inject` | `inject_lordos.go`: `Injector` over `lordshare`. Key codes to evdev with the table the Wayland backend already has (go-link:`pkg/inject/wayland_linux.go`). Positions in session pixels. `Trusted` answers from `remote-input`. |
| `pkg/cursor` | `cursor_lordos.go` from `RP_CURSOR` and `RP_SHAPE`. "Pointer in the picture" isn't possible with encoded frames: the viewer draws it. |
| `pkg/clipboard` | `clipboard_lordos.go` from `RP_CLIP` and `RP_SETCLIP`. |
| `pkg/awake` | Tagged to `SHARE_AWAKE`. |
| `internal/services` | New `encoded_source.go`, a `MediaSource` for encoded frames, with an optional interface on the sink: `EncodedFrame(codec, key, pts, data)` writes to the viewer's `sampleWriter` (go-link:`internal/services/video.go:208`), and `EncodedAudio` to the Opus track, skipping `VideoFrame`'s encoder per viewer (go-link:`internal/services/stream_service.go:633`). PLI maps to `RP_KEYFRAME`, the bandwidth estimate to `RP_RATE`, and `Hearing` gates `RP_AFRAME`. `pickCodec` (go-link:`internal/services/video.go:166`) gets a variant that never picks High. |
| `internal/services/consent.go` | A `SocketConsent`: sends the request to the app and waits. The app gone means `ErrUnavailable`, so it fails closed as `NobodyToAsk` does (go-link:`internal/services/consent.go:46`). `LockScreen` gets a hook. |
| `internal/appsock` (new) | The app's protocol on fd 3: length-framed JSON, 64 KB at most, one message type per field of `uiOptions` (go-link:`cmd/desk/ui.go:14`): status, code, consent, permissions, chat, files, settings, links. |
| `cmd/desk` | `ui_lordos.go`, `runUI` over `appsock`; `ui_headless.go` gets `&& !lordos`. Under `lordos`, the `--test-*` flags aren't compiled, and `setConsent` is always set (go-link:`cmd/desk/main.go:533`). Configuration comes from the app's data folder. |
| `internal/updater` | Nothing: only the GUI starts it (go-link:`internal/gui/update.go:53`), so a `lordos` build has none. Off by construction. |
| `build/lordos` (new) | nFPM file, `go-link-desk-lordos`, installs to `/usr/lib/lordos-apps/go-link-desk/`, `Depends: lordos (>= 0.8.7.1)`, no maintainer scripts. It doesn't conflict with the Linux package. |
| `lordos-app/` (new) | The React app, built with a pinned `tools/lpk.mjs`, which needs a small "sign an unpacked tree" mode. |
| `frontend` | The `in_use` reason, the `/privacy/lordos` page. |

## 9. Packaging, Store and updates

1. **Published first at desk.go-link.org:** both `.deb` files with `SHA256SUMS` and its Ed25519 signature by the go-link update key
   (go-link:`docs/security.md:37`). Today releases live on the release host, so this is new hosting. Never re-uploaded: a fix is a new version.
2. **Catalog:** a `"type": "deb"` entry in `apt/catalog.json` with the version and SHA-256 per architecture (`apt/README.md:54`). Today
   `do_deb` checks the pinned hash and control fields (`apt/build.sh:285-296`). It gains an optional signature check, `sums` and `sig`
   URLs and a key in `apt/keys/`, in the same spirit as sigstore for codex-cli (`apt/README.md:98`).
3. **Mirrored byte for byte:** `pool/catalog/`, append-only, with `apt-repo.sh` refusing a different file under a published name
   (`docs/UPDATES.md:334-336`). Vendor packages are never modified (`apt/README.md:101`).
4. **Store:** a new catalog section, `remote`, since it's neither AI nor commercial (`apps/store/ai.js:137`, `apps/store/index.jsx:345`), with
   the go-link name and vendor and a line saying it's a third-party app and how it differs from 5673. Recommended there and nowhere else: no
   preinstall, no dependency of `lordos`, no first-run nudge. On a system below 0.8.7.1 the page says to update LordOS first.
5. **Updates:** through APT and Store → Updates, as for any catalog app.
6. **Privacy policy:** its own page at desk.go-link.org, linked from the Store item and from the app's About. It says what signalhub
   and TURN see, what stays on the computer, and that the app has no analytics.

## 10. Threat model

| Threat | What's there | What remains |
|---|---|---|
| Signaling server reads the session | Media and channels are DTLS end to end (go-link:`docs/security.md:20`) | It sees who connects and the codes |
| Signaling server in the middle, rewriting the SDP on both sides | Linked devices are protected: the proofs cover the fingerprints (go-link:`docs/security.md:82`). The person sees the request and decides | **Residual DTLS risk:** a code session has no such proof. The visitor's name is only declared. A server that controls both sides can read a session the person accepted, believing it was someone else. Proposed fix: a short code from both fingerprints, read out by voice, D5 |
| A fake visitor | Knock, 60-second question, one at a time, name shown as declared (go-link:`docs/security.md:26-31`) | The scammer on the phone, go-link's own warning and panel stand against it |
| TURN relay | Only DTLS and SRTP | Size and timing |
| A bug in Pion or the engine, reached from the network | The sandbox of 3.3: no home folder, no `AF_UNIX`, no exec. The engine doesn't parse video | While a share runs, the attacker holds what fd 4 grants: the picture and any input given. The indicator and Stop remain, and system surfaces and protected windows stay out of reach |
| A visitor with control abuses the desktop | Permission boxes, the panel, Stop, 4.7's drops | They can do what the person can do in ordinary windows. That is what control means |
| A visitor presses Allow, or changes their own permissions | System surfaces take no API input | The app's own panel is an ordinary window: drop injected input to the app's own windows too. To decide in stage 3 |
| Another app abuses the API | Scope identity, prompt, root-owned store, indicator, no frames without it, one holder, no resize on a monitor session, rate limits, never blocks the compositor | **An unsandboxed Linux app runs as the user.** It can attach to an approved app, or edit its files. The permission is consent, not a sandbox, for Linux apps. Their isolation is what it is (`docs/PRIVACY.md:40`) |
| An app hides or fakes the indicator | Drawn by the shell, `notification` layer, frames only after it shows | A fullscreen surface's own drawing can imitate one; it can't remove the real one. `xdg_popup`s paint above everything (`docs/LAYERS.md`): to test that one can't sit on the badge |
| Same-uid access to `remote.sock` | Unchanged today, section 2 | Open decision D6: refuse non-root there |
| Links in `device.json` | Under the app's data folder, 0600 | An unsandboxed app of the same user can add one. D3 weighs this |
| A forged package | Signature at desk.go-link.org, pinned hash, LordOS archive signature, Verified author check at each load, revocation | The go-link release computer, and the catalog's reviewer |
| A locked session seen | Ended in `lord-server`, not by the client | A frame already in a socket or a viewer's decoder |
| Secrets through the clipboard | Sensitive items and earlier copies never go | What the person copies during the session |
| A viewer recording | go-link's own rule: a notice for honest viewers (go-link:`docs/security.md:71`) | A dishonest one records anyway |

**Not covered:** root on the machine, a compromised kernel, or the person accepting a scammer knowingly.

## 11. Staged plan

Estimates are estimates, one person full time, not commitments. Nothing below has started. The stages run in order except where noted.

| Stage | Work | Done when | Estimate |
|---|---|---|---|
| **0. Proof of concept** | `lord-desk`, a throwaway Go program on a test VM with a test session and no real data: connects to `remote.sock` as the same user (finding 2), sends `RP_VIDEO`, reads `RP_VFRAME`, feeds Pion through go-link's signaling, with the measurement build's auto-accept. No API, no app, no sandbox | A browser at desk.go-link.org shows the session live with the code, H.264 and VP8; a lost frame recovers through `RP_KEYFRAME`; a protected window is black; the encoder stops when the viewer leaves; latency, CPU and bit rate written down | 3–5 days |
| **1. API** (LordOS 0.8.7.1) | `share.c`, `share.sock`, permissions, `share-grant`, indicator, stop shortcut, section 5, input rules; tests and fuzz run as for 5673 | A test client passes every state of 4.6; every row of section 5 on a VM, with the page's message; a browser on 5673 behaves as before when no share exists (the 5673 tests, and the same measurements before and after); revoke cuts within a second | 3–4 weeks |
| **2. Companion** (LordOS 0.8.7.1, needs D1) | Third root, `BOP_COMPANION`, `sandbox_exec_companion`, catalog check | A test package with a Go binary runs with Pion working; `sandbox-test` style checks inside it: no home, no `AF_UNIX`, no exec; a changed byte refuses to start; the same manifest in a home `.lpk` is ignored | 2 weeks |
| **3. Engine** | Section 8's packages in go-link | `make check` passes for other platforms with no change in their files beyond build lines; go-link's tests pass; a headless run against the API serves a browser, with acceptance answered over the socket, and the app gone means refusal | 2–3 weeks |
| **4. App** | React app and all pages | The go-link functions of section 7 work on amd64 and arm64, in all themes, right to left included | 2 weeks |
| **5. Measurements and review** | VM 116, a real GPU with VA-API, Raspberry Pi 5; a security review of the API and companion; an attack run like `docs/REMOTE.md`'s | Numbers per machine in this document; findings fixed; go/no-go on claims for each machine | 1–2 weeks |
| **6. Delivery** | Section 9, the privacy page, the Store text | A signed version installs from the Store on a clean 0.8.7.1, updates through APT and uninstalls clean; the file in the pool is byte-identical to the one at desk.go-link.org | 1 week |

Stages 1 and 2 are what LordOS 0.8.7.1 must carry, and they are about 5–6 weeks by estimate. If only stage 1 fits, the app's `Depends`
moves to the release with the companion. Machines isn't blocked either way. Stage 5 needs hardware with VA-API, which has never been
measured.

## 12. What has been tested

**Tested:** nothing. No line of this has run.

**Read, and relied on:** everything cited above, as of commit `a621d38e` of LordOS and `1203f15` of go-link Desk.

**To test first,** because the design leans on them: the pure-Go engine build; Pion under the seccomp list; UI and engine in one scope's
limits; Baseline at the session's size against the `42e01f` line both sides offer; mono Opus in go-link's track; the indicator above every kind of
fullscreen, direct scanout included; x264 on the Pi.

## 13. Open decisions

- **D1. Packaged apps with a companion (blocks stage 2 and the app).** Accept 3.2: a third read-only app root, a Verified-author check at
  load, and a fourth exception to "the server doesn't run commands". The alternative is an unsandboxed Linux app, which has no native UI.
- **D2. Files.** `~/Downloads/go-link` only (proposed), or read access to the whole home folder as on Linux, which gives a compromised engine
  `.ssh` and the rest.
- **D3. Unattended access.** Keep as on Linux, off by default (proposed), or leave it out at first. With it on and no monitor, nobody sees
  the indicator.
- **D4. Open at login.** Without it, no code and no knocks until the app is opened, and unattended access only works while it's open.
  Is a per-app "open at login" through the broker wanted?
- **D5. Short verification code** against the residual DTLS risk: it changes go-link's website and the panel on every platform, so it's
  go-link's call. Proposed as an option.
- **D6. Close the same-uid door on `remote.sock`.** Every real client connects as root: the bridge connects before dropping root
  (`server/src/remote/conn.c:1246`, `:1278`), and the only tests that use it run with `sudo` (`server/tests/gpu_cycle.sh:50`). Refusing other uids
  looks free and doesn't change what 5673 does, but it touches 5673's socket, so it's for the maintainer to approve. Until it's done, the
  system permission can be bypassed by any unsandboxed app of the user. Proposed for 0.8.7.1, after stage 0.
- **D7. The numbers:** Stop's shortcut, the 2 s indicator wait, keyframe and input limits.
- **D8. Scope limits** for a packaged app, if stage 2 shows one core or 512 MB isn't enough.
- **D9. Assistants and the terminal.** Their views are part of the copy. Running them needs a wider sandbox than the rest of the app; how is to be decided.
