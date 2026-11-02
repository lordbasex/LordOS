# Screen-sharing API

The public door a third-party app uses to share the session: encoded frames, Opus, input and the clipboard, with the person's
consent. It's stage 1 of [GO-LINK.md](GO-LINK.md), general, and knows nothing about go-link. It isn't remote desktop on port 5673
([REMOTE.md](REMOTE.md)): it has its own socket, client table and parser, and shares only the composed frame, the session's encoder and
the media and input formats. Code: `server/src/remote/share.c`, `sharewire.c` (format and limits), `sharegrant.c` and `sgrant_admin.c`
(grants), `server/src/ui/js_share.c`, `sdk/share.js`.

## Who may connect

`$XDG_RUNTIME_DIR/lordos/share.sock`, 0600, created only by a user session. **The login screen's `lord-server` doesn't create it.**
The app's id comes from the peer's root-owned scope, read through `SO_PEERPIDFD` so a dead pid can't be reused, and is read again at
each start. A process in no app scope gets `no_identity` and is closed: Terminal programs, `systemd-run --user`, anything the launcher
didn't start. A kernel without `SO_PEERPIDFD` gets the same answer. A package's id is never taken from a socket: its process can't make one, and a Linux app
whose scope reads `lpk-<x>` gets `no_identity`. A packaged app's companion doesn't use the socket: lord-server connects it at start and hands it fd 4, tied to `lpk:<id>`, only if
the manifest declares `screen-share`; asking for input or the clipboard without `remote-input` in the manifest gets `denied` ([PACKAGED-APPS.md](PACKAGED-APPS.md)).

## Messages

Framing is `proto.h`'s: u32 length, u8 type, data. Nothing is lenient: a wrong length, a value out of range or an unknown type ends the
connection, and so does any seat message (`RP_DISPLAY`, `RP_KEYRING`…).

| Client → system | Data |
|---|---|
| `SHARE_START` 50 | u8 flags (1 video, 2 audio, 4 input, 8 clipboard; video is required), u8 codec (1 H.264, 2 VP8), u16 Hz (24–240), u32 kbit/s (300–20000) |
| `SHARE_QUERY` 51 | none: answers `SHARE_STATE`, prompts nobody |
| `SHARE_STOP` 52, `SHARE_LOCK` 54 | none |
| `SHARE_AWAKE` 53, `SHARE_HEAR` 56 | u8 on. `RP_AFRAME` is dropped until `SHARE_HEAR` 1 |
| `SHARE_TEXT` 55 | UTF-8, 1 KB at most, typed through the active layout; what no key can type is dropped |
| `RP_KEYFRAME`, `RP_RATE`, `RP_RESIZE`, `RP_PTR`, `RP_BTN`, `RP_WHEEL`, `RP_SCROLL`, `RP_KEY`, `RP_SETCLIP` | as in `proto.h`, checked exactly |

| System → client | Data |
|---|---|
| `SHARE_STATE` 60 | u8 why a start would be refused (0 it wouldn't), u8 granted, u8 codecs, u8 0, u16 w, h, hz |
| `SHARE_REFUSED` 61, `SHARE_END` 62 | u8 reason |
| `RP_SIZE`, `RP_VFRAME`, `RP_AFRAME`, `RP_CURSOR`, `RP_SHAPE`, `RP_CLIP`, `RP_VINFO` | as in `proto.h` |

Reasons: 1 `no_identity`, 2 `denied`, 3 `no_person`, 4 `no_indicator`, 5 `busy`, 6 `locked`, 7 `away`, 8 `remote_desktop`, 9 `restricted`,
10 `revoked`, 11 `stopped`, 12 `bad_request`, 13 `ended_by_app`.

## Consent

Two permissions, `screen-share` and `remote-input`, in `/var/lib/lordos/share/<uid>.json`: root's, 0640, group the user's. The closed
actions `share-grant <id> <perm> allow|deny|ask [exe]` and `share-forget <id>` write it, for the session user only; they are reachable only
from `lord-server`, and `lord.admin` exists only in the shell's context. `lord-server` reads the file and accepts it only if root owns it and
neither group nor others can write it. A damaged file grants nothing.

A Linux app's grant is tied to its program: another path asks again. A path may hold letters, digits and `/._-+@~`, with no `..`; a program
elsewhere can't hold a grant. Every string in the file and in the events to the shell goes through `json_quote`.

With no decision the system draws the prompt, in the `system` layer. If no one can answer, the answer is `no_person`, never a default. The
prompt only records a decision: the share goes on from what the root-owned file says. Revoking is read within a second, without the shell.
`FW_R_NO_SCREEN` refuses without a prompt.

## The indicator

`ShareBadge`, in the `notification` layer of every theme, with the app's name and **Stop**. No frame leaves until the shell says it's on
screen, and with no answer in 2 s the share ends with `no_indicator`. It ends the same way if the badge goes or the theme changes.
⌃⌥⇧Esc ends a share in C, on the physical keyboard only ([SHORTCUTS.md](SHORTCUTS.md)).

## The lock

The session goes on drawing behind the lock, so nothing outside can be relied on to stop it. `share.c` keeps its own state, set
synchronously in the call that changes it, before another frame is composed:

| Event | Where | Result |
|---|---|---|
| Lock | `mirror_session` | `SHARE_END {locked}` |
| Switch user | `mirror_session` | `SHARE_END {away}` |
| The monitor leaves (`RP_UNDISPLAY`) | `mirror.c` | `away`, or stays `locked` |
| `lord-sessiond`'s seat connection ends | `mirror.c` | `away`, or stays `locked` |
| The monitor is lent (`RP_DISPLAY`) | `mirror.c` | clears: lord-sessiond lends it only after sign-in |

While the state is set: what is queued for the client is thrown away except the rest of a message already on its way, the encoder stops, no
frame, sound, cursor or clipboard goes, input is dropped, and every `SHARE_START` is refused with that reason. Held keys and buttons are
released. A session locked with no monitor stays locked until one is lent.

## One door at a time

Per session, decided on the main loop. A 5673 viewer is a root connection on `remote.sock` that said `RP_HELLO`. With one connected,
`SHARE_START` gets `remote_desktop`. With a share running, `RP_HELLO` gets `RP_BUSY {app}`: that connection receives nothing else and what it
sends is ignored, the bridge sends `{"t":"bye","busy":…}`, and the page says so in English, Spanish or Portuguese and tries again every 30 s.
The sign-in stays valid. Neither door can end the other. With no share, these paths are as they were.

## Input

Through the same functions as 5673, with this door's own rules. Dropped: anything with no running share or no `remote-input`; anything while the
`overlay`, `modal` or `system` layers show something (read from the node tree in C, not from JavaScript); presses, clicks and wheels aimed at
a protected window. A release always goes, so nothing stays held. So a visitor who opens the search can't type in it or close it: the person
at the computer has to. Limits: 1,000 pointer events and 200 keys a second.

## Keyframes

The encoder makes one only when asked. At most one every 500 ms always. If the screen didn't change since the last one, a request spends a token:
3 at most, one more every 30 s. A request is never lost, it waits its turn; a first request or a new viewer after a quiet spell is served at once.
So a viewer asking every 3 s on a still screen costs about 2 keyframes a minute, not 20. On a changing screen only the 500 ms applies.

## Clipboard

Only with the `clipboard` flag and `remote-input`, text only, starting from what is copied after the start. Items marked sensitive never go.

## What was tested

On VM 116 (amd64, 2026-11-01), against isolated headless sessions with a test client in a real app scope, CPU composition:

- `server/tests/share_e2e.sh`, 55 checks passed: no socket on the login screen; `no_identity`, `no_person`, `denied`, a grant for another program, a
  damaged file and a file others can write; H.264 and VP8 frames after the indicator; `busy`; input with and without a grant; lock by
  `SHARE_LOCK` (no frame, input not reaching the session, starts refused, encoder stopped); `RP_UNDISPLAY` and the seat ending; the unlock by
  `RP_DISPLAY`, with a dummy device; every row of the one-door rule against a root connection acting as the bridge; revoking (about 600 ms);
  12 keyframe requests on a still screen gave 3 keyframes; system surfaces, with the search open; the clipboard's start; malformed messages and
  200 connections of random bytes; the socket removed at exit; no `lord-media` zombie.
- `share-test`, 298 checks; `share-fuzz`, 200,000 iterations under ASan and UBSan; `share_model_test.mjs`. The hour of 3 s requests (122 keyframes) is
  simulated, not live.
- `remote.sock`: a process of the same user is closed at once, root is served, and a client that connects as root and then drops to the user
  stays connected, which is the bridge's order. The real `lord-sessiond` was not run.

**Not tested:** the system prompt and its answer (a headless session has no person to ask); a real monitor, a real lock screen and a real
`lord-sessiond`; the real bridge and page with `RP_BUSY`, and a browser on 5673 before and after; ⌃⌥⇧Esc; a click on the badge's Stop; protected
windows; sensitive clipboard items; sound; `SHARE_TEXT` reaching a field; `FW_R_NO_SCREEN`; several displays; the GPU path; a badge under an
`xdg_popup`. Settings and Monitor additions were built, not looked at.

Uninstalling forgets a grant: Remove calls `share-forget`, and at each session start the shell forgets the grants of apps that are gone, for what dpkg removed. The rule is in
`share_model_test.mjs`; the admin action wasn't run from a real shell.
