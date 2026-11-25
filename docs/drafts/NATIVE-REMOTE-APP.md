# A native LordOS remote-desktop app on the go-link Desk network (design study)

**Status: draft, design only.** Nothing here is built. It proposes a new native app, exclusive to LordOS and maintained in this
repository, that speaks the go-link Desk network (signalhub, its TURN relays, the 9-digit codes, links and invitations) so that
the desk.go-link.org web viewer and the existing go-link Desk apps (macOS, Windows, Linux, iOS, iPadOS, Android) can connect to it
unchanged, and so that it can connect to them. Everything on the LordOS side is written from scratch in C and JavaScript: no Go
engine, no Pion, no code copied from go-link Desk. The working name in this document is **Lord Beam** (section 8 lists the
candidates); read it as a placeholder.

How to read it, as in [GO-LINK.md](../GO-LINK.md):

- **Read** means checked in code or documents on 2026-11-25, with the place given. It says what the code says, not that anyone ran it.
- **Measured** is a number already published in LordOS's documents, on the machine named. **Target** and **estimate** are goals and guesses.
- Paths without a prefix are LordOS's (`test/integration-08961` at `13ee93136`). `go-link:` is the go-link Desk repository at
  `feat/lordos-app` `bba160b` (go-link Desk 1.8.1), under `backend-device/` unless it starts with `docs/`, `frontend/` or `protocol/`.
  `signalhub:` is `github.com/lordbasex/signalhub` at `501ed47`. The phone apps are in go-link Desk's `main` (`822cf6f`), `mobile/`.

## 0. Summary

- **Contract.** The app must use signalhub v1 with `app: "go-link-desk"` (codes are namespaced by `app`, so any other name would
  make the web viewer and the phones unable to find it), act as the **WebRTC offerer** that creates the `control`, `pointer`,
  `input` and `files` data channels, send H.264 Constrained Baseline (or High) or VP8 and stereo Opus, and implement go-link's
  knock/accept, control messages, input packets, files protocol and ECDSA P-256 link proofs byte for byte. The iOS/iPadOS and
  Android apps connect unchanged if the host also sends `welcome.host.os: "linux"`, cursor PNGs, boolean permissions and
  `accepted` only after a good password; phones can't use invite links or calls, so the 9-digit code stays first-class. Section 1.
- **Stack: grow LordOS's own WebRTC peer, don't add a library.** LordOS already has a C WebRTC peer for remote desktop 5673
  (`server/src/remote/rtc.c`, `rtcwire.c`: ICE-lite, DTLS 1.2 with OpenSSL, SRTP with libsrtp2, SCTP data channels with usrsctp, RTP
  packetizers, a TURN client over UDP/TCP/TLS, 387 unit checks and a fuzzer). It lacks what an outbound, NAT-traversing offerer
  needs: a full ICE agent (server-reflexive candidates, sending checks, nomination, consent freshness), an SDP *offer* writer,
  opening data channels, transport-wide congestion control and receiving RTP (for calls and for the viewer). Recommended: factor it
  into a shared `server/src/rtc/` library used by both the 5673 bridge and one **new sandboxed network helper, `lord-beam-net`**.
  No new library; three existing approved dependencies (OpenSSL, libsrtp2, usrsctp) get one more binary each. Section 2.
- **Architecture.** The app UI is a LordOS system app in JavaScript. `lord-beam-net` speaks signalhub, WebRTC and the go-link
  messages in a network-only sandbox with no file access. Picture, sound and input go through the existing **screen-sharing API**
  (`share.c`), so the permission prompt, the indicator, the lock rule, protected windows, the input drop rules and the one-door rule
  with 5673 are all reused as they are. Encoding stays in `lord-media` (VA-API H.264 on the RX 580, x264 elsewhere, VP8, Opus). Files
  are opened by `lord-server` under `lord.fs`'s rules and handed over as descriptors. Section 3.
- **Targets.** RX 580 PC: 1920×1080 at 60 fps, VA-API, glass-to-glass under 50 ms median on a LAN, under 25 % of one core for
  everything LordOS adds. Raspberry Pi 5: 1280×720 at 60 or 1920×1080 at 30 with x264, under 80 ms on a LAN. Section 6.
- **Effort.** About 5 to 7 person-months to parity with Desk go-link, of which host-only sharing with the web viewer (the first
  thing that can ship) is about 2.5 to 3. Section 7.

## 1. The go-link network contract

Everything in this section is what the native app must say and expect *exactly*. go-link's own documents call
`signalhub:README.md` "the contract" for signaling and `go-link:docs/protocol.md` the description of everything after it; a
large part of the WebRTC profile is only in code. Each row says which.

### 1.1 Signalhub v1

**Read** in `signalhub:README.md` (documented, the contract) and `go-link:frontend/packages/shared/src/protocol.ts`.

| Item | Value | Source |
|---|---|---|
| Endpoint | `wss://signal.go-link.org/ws?v=1`; one JSON object per WebSocket message; a 64 KB read limit | documented |
| Namespace | `app: "go-link-desk"` (`APP`, `protocol.ts:9`). A code or invite of one `app` is invalid in another, and `ALLOWED_APPS` on the server may refuse an unknown one. The new app **must** use `go-link-desk`. | documented + code |
| Envelope | `type, app, device_id, device_secret, code, room_id, public, invite_only, invite, meta, rooms, session_id, peer_id, remote, to, from, ice_servers, payload, error`; empty fields omitted; `from` is set by the server | documented |
| First message | `hello {peer_id, ice_servers}`; `ice_servers` has the shape of `RTCIceServer` (`urls[]`, `username`, `credential`) | documented |
| Device identity | `register {app, device_id (UUID), device_secret (32 random bytes, base64url, 43 chars)}` binds the id to `SHA-256(secret)` for 30 days after the last register, in memory only; another secret gets `device_id taken` | documented |
| Pairing code (unattended linking) | `register` answers `code {"113 134 323"}`: 9 digits, **10 minutes, single use**; a browser redeems it with `claim {app, code}`, both get `paired` | documented |
| Session code and link | `room_open {invite_only: true, meta: {name, os}}` → `room_opened {room_id}`; `invite_create {room_id}` → `invite_created {room_id, invite, code}`. `invite` is 128 random bits in base64url (22 chars), `code` 9 digits. **No expiry**; a new `invite_create` replaces both. One room per session by default | documented |
| Joining | `join {app, code}` or `join {app, invite}` → guest gets `joined {session_id, remote, room_id}`, owner gets `peer_joined {session_id, remote, room_id}` | documented |
| Coming back | `reach {app, device_id}` → browser `paired`, owner `reached`; proves nothing | documented |
| Relay | `signal {to, payload}` / `signal {from, payload}`; star topology: guests may only signal the owner | documented |
| End | `peer_left {session_id, from}`; the owner leaving closes the session and its rooms | documented |
| Errors | fixed strings (`invalid or expired code`, `rate limit exceeded`, `device_id taken`, …), to be compared as text | documented |
| Limits | `claim`/`reach`/`join` 5 a minute per IP; `register`/`room_open`/`invite_create` 30 a minute per IP; 30 messages a second per connection (then closed); 20 connections per IP; outgoing buffer 32 messages, dropped when full | documented |
| Origin | native clients send no `Origin` and are always accepted | documented |
| `panel`, `panel_nonce`, `token`, `nonce`, `proof`, `mode` | in go-link's TypeScript types but **not in signalhub's README**: they belong to go-link's local web panel. Not needed. | code only |

**TURN credentials.** signalhub hands out TURN REST credentials in `hello`: `username = <expiry>:<peer_id>`, `credential =
base64(HMAC-SHA1(TURN_SECRET, username))`, 12 hours by default (documented). The relay is coturn next to signalhub (Virginia;
São Paulo planned, `go-link:docs/network.md`). The app uses them as long-term credentials (realm from the 401), which
`rtc.c`'s TURN client already does for the 5673 relay. STUN comes from the same `hello` (`stun:stun.l.google.com:19302` is
signalhub's default). The credentials are tied to *this* WebSocket's `peer_id`: a reconnect to signalhub means new ones.

**Stability.** signalhub's protocol is versioned in the URL (`?v=1`) and its README states it is the only contract. go-link's own
rule is never to patch signalhub for Desk (`go-link:docs/signalhub-proposals.md`). This is the most stable layer.

### 1.2 Session start (inside `signal.payload`)

**Read** in `go-link:docs/protocol.md` §2 (documented) and `internal/services/session_service.go`.

```
viewer → host   {"kind":"knock","name":"Ana","ua":"Chrome 141 on macOS","codecs":["h264-high","h264","vp8"]}
host   → viewer {"kind":"accepted","permissions":{"view":true,"control":false,"audio":false,"clipboard":false,"files":false,"record":false}}
host   → viewer {"kind":"rejected","reason":"declined"|"timeout"|"busy"|"unavailable"|"off"}
host   → viewer {"kind":"offer","sdp":"…"}          only after accepted; again for an ICE restart
viewer → host   {"kind":"answer","sdp":"…"}
both            {"kind":"candidate","candidate":{…}} trickle ICE (RTCIceCandidateInit shape)
```

Rules the host must keep (all documented): 60 seconds to decide, then `timeout`; one session and one pending knock at a time
(`busy`); one knock per guest per signalhub session; never send an offer to, and ignore every SDP and candidate from, a viewer
that wasn't accepted; `name` cut to 40 and `ua` to 60 printable characters with no line breaks, control or format characters
(bidi overrides), shown as *declared*; codec choice: H.264 High if the host has an H.264 encoder and `codecs` has `h264-high`,
else H.264 Constrained Baseline if it has `h264`, else VP8; no `codecs` field means VP8. An ICE restart is a new offer with new
ICE credentials after 3 s `disconnected` or on `failed`, at most 3 in a row, then `session_end {reason: "network"}`. Signals
must be applied strictly in order.

LordOS 0.8.8's port added `rejected {reason: "in_use"}` (the session is held by 5673 or another app); older viewers show a
generic refusal (GO-LINK.md §5). The new app sends it too.

### 1.3 Unattended access and device keys

**Read** in `go-link:docs/protocol.md` §3 (documented in detail) and `pkg/linkauth/linkauth.go`, `internal/services/link_service.go`;
the cross-implementation vectors are `go-link:protocol/vectors/link-proof.json`.

- The host has one long-lived **device key**, ECDSA P-256. Public keys on the wire: 65-byte uncompressed point, base64url
  without padding; signatures: 64-byte P1363 `R‖S`, base64url; ES256 over SHA-256.
- Signed messages: `"go-link-desk host proof v1\n" + nonce + "\n" + host fingerprint + "\n" + viewer fingerprint`, and the same with
  `link proof v1` (the browser's) and `host unlink v1` (a signed refusal). A fingerprint is the first `a=fingerprint:sha-256` line of
  that side's SDP, written `sha-256 AB:CD:…` in upper case. Because both proofs cover both DTLS fingerprints, a signalhub that
  rewrites SDP to sit in the middle is caught.
- **Linking** (`claim` with the pairing code): the knock is asked as a *link* request; on `control` the browser sends `auth {pub}`,
  the host answers `auth_ok {device_id, link_id, host_key}`, keeps only the public key, name and permissions.
- **Coming back** (`reach`): the host connects the browser **gated** (nothing in or out, not even `welcome`) for at most 30 s;
  `auth_challenge {link_id, nonce}` (16–128 chars) → `auth_proof {link_id, sig}` or signed `auth_failed {sig}` → `auth {link_id, sig}`
  → `auth_ok` | `auth_failed`; then `password_required {}` → `password {password}` → `password_result {ok, reason, left,
  retry_after}` (`wrong`/`blocked`/`locked`; 5 tries per browser, 20 wrong in 10 minutes lock all tries for 10 minutes; 2 minutes to
  type); then `accepted {permissions}` over signalhub. Unattended access always has a password, kept as an argon2id hash.
- **Revoking:** `unlinked` on an open session, `session_end {reason: "revoked"}`; later `reach`es get a signed `auth_failed`. A viewer
  may send `unlink {}`.
- **Breaking history:** go-link replaced an HMAC-token scheme with this one "with no fallback" (protocol.md §3). `go-link:docs/security.md`
  rule 4 and the signalhub README still describe the HMAC scheme: **stale text, the code and protocol.md are the truth.**

### 1.4 The WebRTC profile the viewers expect

This is the part that is **only in code** (`go-link:internal/services/stream_service.go:94-148`, `:952-1120`, `video.go:156-190`,
`audio.go:82`, `call_tracks.go`, `playout_delay.go:26`; viewer side `go-link:frontend/packages/shared/src/stream.ts`). The host is
Pion; the new app has to produce an offer the same viewers answer.

| Item | What go-link's host does | What the new host must do |
|---|---|---|
| Roles | Host always offers, creates every data channel, `a=setup:actpass`; the browser answers `active`, so the host is the **DTLS server** | same. `rtc.c` is already a DTLS server |
| Bundle | one transport (Pion's default max-bundle), `rtcp-mux` | same; one UDP 5-tuple per viewer |
| m-lines, in order | screen video (send, stream `go-link-desk`), screen audio (send, track `audio`, stream `go-link-desk`), call camera (send, track `camera`, stream `go-link-desk-call`), call mic (send, `mic`, `go-link-desk-call`), call video (recv), call audio (recv), then `application` (SCTP) | same order and stream ids. The web viewer tells call tracks apart **only** by `streams[0].id === "go-link-desk-call"` (`stream.ts:405`, `:513`) |
| Video codecs | Pion's defaults plus H.264 Constrained High `profile-level-id=640c1f` PT 114 with RTX PT 115; Baseline `42e01f`; VP8. Feedback `goog-remb`, `ccm fir`, `nack`, `nack pli` | offer the chosen codec (`640c1f` or `42e01f` with `packetization-mode=1;level-asymmetry-allowed=1`, or VP8) plus its RTX; the same feedback plus `transport-cc` |
| Header extensions | transport-wide sequence numbers (TWCC); `http://www.webrtc.org/experiments/rtp-hdrext/playout-delay` on video (Chrome honors it) | both. The playout-delay extension is how go-link lowers the browser's jitter buffer (see 3.5) |
| Audio | Opus 48 kHz **stereo**, 96 kbit/s, 20 ms frames, always in the offer, silent until `permissions.audio` | same. LordOS's `mstream` makes mono 64 kbit/s today: mono in a stereo line plays, but stereo is the target (3.6) |
| Call receive line | video recv offers only H.264 packetization mode 1, Constrained Baseline `42e01f` PT 106 (RTX 107) and `42001f` PT 102 (RTX 103) | same; decoded by `lord-media` |
| Data channels | `control` ordered reliable (text JSON); `pointer` `ordered:false, maxRetransmits:0` (binary); `input` ordered reliable (binary); `files` ordered reliable (text and binary); `terminal` only for go-link-desk-terminal. In-band DCEP (not `negotiated`) | same labels and options, opened by DCEP from the host. As DTLS server the host uses **odd** SCTP stream ids (RFC 8832) |
| SCTP | Pion: port 5000, `max-message-size` 65536 | same. A message is at most 64 KiB (protocol.md §4) |
| Message size checks | control and input messages over 64 KiB dropped (`maxControlMessage`) | same |
| ICE | full agent; host candidates, srflx from `hello`'s STUN, relay from `hello`'s TURN; trickle; optional single UDP port (ICE UDP mux) and port mapping (UPnP/NAT-PMP); optional relay-only | full agent (new, see 2.1). Single port and router mapping are later options |

**Stability.** Not versioned. It is whatever Pion's defaults and go-link's additions produce, and the viewers are browsers and
Google's or Apple's WebRTC, which negotiate around differences. What the new host must not break is narrower: the stream ids,
the channel labels and options, the codecs the knock names. A conformance test (section 6.4) offers the new host's SDP to Chrome,
Firefox, Safari and the phones' stacks and records what they answer.

### 1.5 Control channel

**Read** in `go-link:docs/protocol.md` §4 (documented). JSON, at most 64 KiB; an unknown type is dropped.

| Message | Dir. | The new host |
|---|---|---|
| `welcome {host: {name, os}, source, features?}` | H→V | must send it when `control` opens (not while gated). `os: "linux"` (1.9 explains why not `"lordos"`), `source: "screen"`, `features` omitted (it only announces `terminal-tabs`) |
| `displays {displays: [{id, name, width, height, points_width, points_height, primary}], current}` | H→V | from `SHARE_DISPLAYS`; also after each `display_select` |
| `display_select {id}` | V→H | → `SHARE_DISPLAY` (keyframe follows, same track) |
| `ping {id}` / `pong` | H→V→H | every 2 s |
| `stream_stats {fps, width, height, kbps, codec, target_kbps, estimate_kbps}` | H→V | every 2 s |
| `client_report {metrics: {freeze_ms, video_loss_pct, jitter_ms, hidden}}` | V→H | feeds the playout delay and the loss cap |
| `video_want {width, height}` / `quality {preset, retina}` | V→H | size and preset; needs scaling in the encoder (3.4) |
| `cursor {id, png, hot_x, hot_y, width, height, pixels}`, `cursor_pos {x, y, visible}` | H→V | from `RP_SHAPE`/`RP_CURSOR`; PNG made by `lord-server`'s own encoder, never by the helper |
| `cursor_mode {in_video}` | V→H | needs a "draw the pointer into the share" flag in `share.c` (new) |
| `beacon {seq}` | V→H | latency beacon: a 32-point square, white or black, top-left of the shown display, click-through, 4 s after the last flip. Needs a `SHARE_BEACON` message (new; 3.5) |
| `notice {code}` | H→V | `no_remote_input` when the system grant has no input; `share_denied` when the person refused the system prompt |
| `permissions {view, control, audio, clipboard, files, record}` | H→V | on every change in the session panel |
| `clipboard {id, part, parts, text, paste?}` | both | 16 KiB pieces of UTF-8, 1 MiB per text; `paste` asks the host to paste (3.8) |
| `chat {text}`, `typing {on}` | both | 2000 chars, 20 lines, printable only |
| `call_request`, `call_state`, `call_media`, `call_end` | both | phase 4 |
| `screenshot {id}`/`{id, ok}`, `recording {id, on}` | both | consent for the viewer's own captures; `ok` only with `record` once the panel shows it |
| `session_end {reason}` | both | `host`, `viewer`, `network`, `signal`, `idle`, `revoked` |
| `terminal_*` | — | not offered (no `terminal` channel). A LordOS app never runs a shell for a visitor |
| `auth*`, `password*`, `unlink`, `unlinked` | both | 1.3 |

### 1.6 Input packets

**Read** in `go-link:docs/protocol.md` §5 (documented), with shared vectors in `go-link:protocol/vectors/input.json`. Big endian.

| Packet | Channel | Format | LordOS mapping |
|---|---|---|---|
| `0x01` abs | `pointer` | u16 seq, u8 display, u16 x, u16 y (0..65535 over the display) | drop if `seq` is older than the last applied; scale to the shown picture → `RP_PTR` |
| `0x02` rel | `pointer` | u16 seq, i16 dx, i16 dy (pointer lock) | **new**: `share.c` has only absolute moves; add `SHARE_REL` (or apply to the last position with clamping) |
| `0x10` button | `input` | u8 button (0 left, 1 middle, 2 right, 3 back, 4 forward), u8 down, u8 display, u16 x, u16 y; display `0xFF` = where the pointer is | `RP_PTR` then `RP_BTN`. `RP_BTN` has 3 buttons: back/forward need a new value range (`BTN_SIDE`/`BTN_EXTRA`) |
| `0x20` wheel | `input` | i16 dx, i16 dy, u8 mode (0 pixel, 1 line); dy > 0 scrolls down | `RP_SCROLL` (pixel) or `RP_WHEEL` (line) |
| `0x30` key | `input` | u16 keycode, u8 down (0, 1, 2 repeat), u8 mods (shift, ctrl, alt, meta, caps) | keycode is the **index into `go-link:protocol/keycodes.json`** (`KeyboardEvent.code` order) → evdev → `RP_KEY`; apply held modifiers; caps lock not applied |
| `0x31` text | `input` | UTF-8, at most 1024 bytes | `SHARE_TEXT` (1 KB, the same limit) |
| `0x3F` release_all | `input` | — | release everything this visitor pressed |

The key table's *order* is a wire contract. The new app keeps its own table (`KeyboardEvent.code` → evdev) in the same order, with
a test that fails if it differs from the index order in go-link's published `keycodes.json` and the cases in `input.json`. That is
data and a test vector, not code; section 8 asks the maintainer to confirm reading it is fine.

### 1.7 Files

**Read** in `go-link:docs/protocol.md` §6 (documented). On `files`; JSON text messages and binary data; one upload and one download
at a time per viewer; only with `permissions.files`.

- Upload: `begin {id, name, size}`, 16 KiB binary chunks, `end {id}` or `abort {id}` → `upload_result {id, ok, name, error?}`. Written
  to a temporary file in Downloads, renamed when complete, never over an existing file (`name (2).ext`); the name loses folders,
  control and format characters and leading dots; 20 GB at most.
- Browse: `list {id, path}` → `listing {id, path, entries: [{name, dir, size, mtime}]}`, folders first, at most 5000; anything outside
  the home folder, hidden, or through a link out → `files_error`.
- Download: `download {id, path}` → `download_ready {id, name, size}`; `read {id, offset, length}` answered by binary messages
  (8-byte big-endian offset, up to 60 KiB); `done`/`cancel`; `download_error`. The viewer keeps 16 reads in flight.

### 1.8 Video, sound, call and latency behaviour the viewers assume

**Read** in `go-link:docs/protocol.md` §4 *Video*, *Sound*, *Video call*, *Latency* (documented).

- Presets: Speed 1280 wide, 60 fps, 4 Mbit/s ceiling; Balanced (default) 1920, 60, 10 Mbit/s; Quality the display's pixels, 60,
  20 Mbit/s. The picture follows `video_want` unless less than 10 % smaller.
- Only changed frames are sent; after motion stops the last frame is sent again 4 times 0.4 s apart to sharpen it.
- Congestion: GCC from TWCC feedback, bounded by what was actually sent; encoder at 90 % of the estimate; a ladder of 30 fps under
  2.5 Mbit/s, 1280 wide under 1.5, 960 at 20 fps under 800 kbit/s, 640 at 15 under 400. NACK/RTX for loss, PLI for keyframes.
- The viewer's camera and microphone are never decoded or played unless a call is active.

These are go-link's *behaviour*, not wire format: a viewer works with a host that does less, but the experience should match.

### 1.9 The native clients: iOS, iPadOS and Android

The maintainer's requirement: the App Store iOS/iPadOS app and the Android app must connect to the LordOS app **unchanged**.
**Read** in `go-link-desk` branch `main` (`822cf6f`), `mobile/ios/` and `mobile/android/`; the paths below are under `mobile/ios/`
unless they say Android. (`/mnt/HD08TB/github/go-link-ios` is an old copy whose git directory is missing; not used.) The last iOS archive
is 1.0.4 (build 10004.2611030905, bundle `org.go-link.desk.ios`, iOS 26 minimum); App Store 1.0.3 is the last one the changelog names
as published.

**What the phones send and expect**

| Topic | iOS / iPadOS | Android | What the LordOS host must do |
|---|---|---|---|
| WebRTC stack | Google libwebrtc, `stasel/WebRTC` 153.0.0 (`project.yml:18-20`); unified plan, continual gathering | `stream-webrtc-android` 1.3.10 | a standards-compliant offer; no Pion-specific quirk is relied on |
| Role | always the answerer; never offers; every later `offer` is answered on the same connection (renegotiation and ICE restart work); up to 50 early candidates buffered (`Session.swift:348-360`) | same | host offers, as in 1.4 |
| SDP | no munging, no `setCodecPreferences` | stops the `recvonly` call lines when the phone can't encode H.264 (`DeskRtcPeer.kt:105-118`) | offer must carry `a=fingerprint:sha-256` (link proofs read it) |
| Knock `codecs` | hard-coded `["h264-high","h264","vp8"]` (`Session.swift:183`); H.264 by VideoToolbox, VP8 in software | probed: `h264-high`/`h264` only if a decoder has `64…`/`42…`, else `["vp8"]` (`WebRtcEngine.kt:31-38`) | pick from the list; H.264 High or Baseline and VP8 all work |
| Knock `ua`, `name` | `go-link Desk <version> on iOS|iPadOS <version>`; device or chosen name; cut to 60/40 | `go-link Desk <ver> on Android <release>` | shown as declared |
| Doors | `join {code}` (9 digits), `claim {code}`, `reach {device_id}`; **no `invite` door, no link or QR handling** (`Session.swift:290-298`) | same | always show the 9-digit code beside the link: a phone can only type digits |
| Data channels | in-band DCEP only, matched by label; `control`, `pointer`, `input`, `files` kept, **any other label closed** (`DeskRtcPeer.swift:253-277`) | same | never pre-negotiated channels; no `terminal` |
| `control` | text frames ≤ 64 KiB only; binary or larger dropped (`DeskRtcPeer.swift:312`) | same | keep every control message under 64 KiB, cursor PNGs included |
| Tracks | tracks on stream `go-link-desk-call` ignored; for the rest the **last video track wins** (`SessionModel.swift:393-398`) | same | exactly one screen video track |
| Sound | plays only; microphone never opened | same | — |
| `welcome` | reads `host.name` (64) and `host.os` (16); not required to show the picture | same | **`host.os: "linux"`**: `"darwin"` selects Mac shortcuts, any other value Windows-style shortcuts and, unless `linux`/`windows`, a raw label and generic icon (`Keys.swift:98-125`, `Recents.swift:38-45`). `"lordos"` would need a phone update |
| `displays` | read (16 at most); the input `display` byte is the **list index**, not the id | same | map index → `SHARE_DISPLAYS` id |
| `permissions` | values must be JSON `true`/`false` (`Json.swift:238`) | same | booleans only |
| `cursor` / `cursor_pos` | viewer sends `cursor_mode {in_video:false}` and **draws a pointer only from a `cursor` PNG** (`RemoteScreen.swift:156`); shapes cached by id (64, < 300 KB base64) | same | **mandatory**: send the current shape with its `png` to each viewer once its `control` opens, then by id; otherwise the phone shows no pointer (go-link's own bug, CHANGELOG 1.8.1) |
| `cursor_mode {in_video:true}` | sent while the phone records | same | honour it (the new `share.c` flag of 1.5), or the recording has no pointer |
| Sent by the viewer | `video_want` (view size × scale), `quality {preset, retina:false}`, `client_report` every 2 s, `display_select`, chat, typing, clipboard, screenshot, recording, `session_end`; queued until `control` opens | same | — |
| `stream_stats`, `notice` | parsed, not shown | same | send anyway (the web shows them) |
| Pointer | only `0x01` absolute, also in trackpad mode (the phone moves a virtual pointer itself); `0x02` never sent | same | — |
| Buttons, wheel, keys, text | tap left, two-finger tap right, long press drag; wheel `mode` always 0 (pixels); hardware keyboards by HID position → keycode index; on-screen keyboard → `0x31` text, `\n` as Enter; `0x3F` on lock and focus loss | same | as 1.6 |
| Clipboard | 16 KiB pieces, `paste` on the last; incoming needs every part in order | same | as 1.5 |
| Files | as 1.7; ids are strings (`l<n>`, `d<n>`, `u<n>`); download frames over 128 KiB dropped; 30 s reply timeout | same | ids are opaque strings; frames ≤ 8 + 60 KiB |
| Calls | **not supported**: call tracks ignored, no camera or mic | not supported | calls are web-only; never require one |
| Screenshot / recording | captured on the phone, after `screenshot {id, ok}` / `recording {id, on}` within 5 s; needs `record` | same | as 1.5 |
| Link proofs | Secure Enclave P-256 key behind Face ID; 32-byte base64url nonce; same strings as 1.3 (`LinkAuth.swift:57-79`); password ≤ 512 chars, saved in the keychain and sent once by itself | Android Keystore | as 1.3. **Order:** the phone, after `password_result {ok}`, waits for `accepted` over signalhub, so send `accepted` *after* a good password, never before |
| Version gating | none: no `features`, `source`, host version or minimum checked | none | nothing to announce |

**Nothing here forces a change on the phones** as long as the host follows the rows above. Things that would, if the LordOS app
wanted them: invitation links and QR codes on phones (they have no invite door or deep links), a LordOS-specific `os` value (icon,
name, shortcuts), calls with a phone, a terminal channel, and showing `notice` texts. None is needed for parity, and none is planned.

### 1.10 Versioning and how stable the contract is

| Layer | Versioned? | Stability | Where it's written |
|---|---|---|---|
| signalhub | `?v=1` in the URL | high: the README is a contract, changes go through proposals | documented |
| knock/accept/offer | no version field | medium: new `rejected` reasons and fields have been added; unknown fields are ignored | documented |
| link proofs | strings carry `v1` | medium: changed once with no fallback (HMAC → ECDSA) | documented + vectors |
| control messages | `welcome.features` lists extras; unknown types dropped | medium: grows each release | documented |
| input packets | type byte; vectors | high | documented + vectors |
| SDP profile | none | low on paper (Pion's defaults), high in practice (browsers negotiate) | code only |
| keycodes | index order in `keycodes.json` | high: append-only by nature | data file |

The new app should follow go-link's release notes, run go-link's vectors in its own tests, and run an end-to-end check against
the public web viewer and a phone on every LordOS release (section 6.4). Because the maintainer owns both projects, a change in
go-link that would break LordOS can be caught before it ships; that's a process, not a guarantee.

## 2. WebRTC stack options

What every option must provide on top of what the sockets give: a signalhub client (WebSocket over TLS), a **full ICE agent**
(host, server-reflexive and relayed candidates, connectivity checks with pacing, triggered checks, nomination, ICE restarts,
consent freshness, RFC 8445/7675), a TURN client with REST credentials (RFC 8656), DTLS 1.2 with ECDSA certificates and
fingerprint checks, SRTP and SRTCP (RFC 5764), SCTP over DTLS with DCEP and partial reliability (RFC 8831/8832, PR-SCTP for
`pointer`), an SDP offer writer and answer parser, RTP packetizers and depacketizers for H.264, VP8 and Opus, RTX, NACK, PLI/FIR,
TWCC (send-side sequence numbers, feedback parsing, an estimator and a pacer) and the playout-delay extension.

### 2.1 What LordOS already has

**Read** in `server/src/remote/rtc.h`, `rtc.c:1-24`, `rtcwire.h`, `docs/REMOTE.md` "Video (WebRTC)", `docs/ARCHITECTURE.md:106`.

| Piece | In `rtc.c`/`rtcwire.c` today | Missing for go-link |
|---|---|---|
| ICE | ICE-lite (answers checks only), built-in STUN responder, ICE-TCP passive, restarts as answerer | **a full agent**: gather srflx, send checks, pair states, nomination (the offerer is the controlling agent), consent freshness. ICE-lite cannot cross the host's NAT, which is the normal case here |
| TURN | client over UDP, TCP, TLS; long-term credentials; permissions for the browser's candidates | REST credentials from `hello` (same mechanism), refresh before 12 h, ChannelBind for lower overhead |
| DTLS | OpenSSL, server role, P-256 per peer, ECDHE-ECDSA AES-GCM/ChaCha20, fingerprint bound to the offer | nothing new (fingerprints are now bound to the *answer*) |
| SRTP | libsrtp2, AES-GCM or AES-CM-HMAC-SHA1-80 | nothing |
| SCTP | usrsctp without threads, timers from `rtc_tick`; DCEP parsing (`rw_dcep_open`) | **opening** channels (DCEP OPEN/ACK as the opener), odd stream ids, PR-SCTP `maxRetransmits:0` |
| SDP | strict offer parser, answer writer | **offer writer**, answer parser, the m-line layout of 1.4 |
| RTP out | H.264 (`rw_h264_packetize`) and VP8 packetizers, Opus | RTX on its own SSRC, the TWCC and playout-delay extensions |
| RTP in | none (the browser sends no media) | jitter buffer and depacketizers for the call (H.264, Opus) and the viewer (H.264 High, VP8) |
| RTCP | NACK, PLI, FIR, receiver reports, REMB | transport-cc feedback; sending RR/NACK/PLI as a receiver |
| Rate | loss-based from RR, REMB as a ceiling (300 kbit/s–12 Mbit/s) | delay-based estimate from TWCC (GCC-style) and a pacer |
| Tests | `remote-sec-test` 387 checks with a real Chrome offer, `remote-fuzz` under ASan/UBSan | the new parsers and state machines |

That is most of the hard, security-sensitive part (DTLS, SRTP, SCTP and the parsers), already reviewed, attacked (REMOTE.md
"Security analysis (0.8.4, video)", 13 attacks) and approved. What's missing is protocol logic, not cryptography.

### 2.2 The options

Effort is one experienced C developer, network layer only (signalhub client to data channels and RTP), not the UI or the go-link
messages, which every option needs equally (about the same in all).

| | (a) Everything our own, no libraries | (b) Vetted C libraries: libjuice (ICE) + libsrtp2 + usrsctp, or libdatachannel | (c) Crypto only through OpenSSL, in a separate sandboxed helper like `lord-tls` | **(d) Grow `rtc.c` into a shared full-ICE peer (recommended)** | (e) Something else: GStreamer `webrtcbin`/libnice, a WPE WebKit page, Pion |
|---|---|---|---|---|---|
| What it means | own DTLS 1.2 record and handshake, own SRTP, own SCTP (association, congestion, PR-SCTP, DCEP), own ICE/TURN; crypto primitives from OpenSSL's libcrypto or Monocypher | libjuice for ICE/TURN (C, small); libdatachannel is C++17 and bundles libjuice, usrsctp and libsrtp | a crypto helper holds the DTLS keys and does SRTP per packet; the network helper does everything else | one library, `server/src/rtc/`, with a full ICE agent, offer writer, DCEP opener, RTP receive, TWCC; used by the 5673 bridge and by `lord-beam-net` | big frameworks or another language |
| Effort (estimate) | 9–14 months (SCTP alone is months; DTLS another 2) | libjuice: 6–8 weeks to integrate into `rtc.c`'s poll loop and replace ICE-lite. libdatachannel: 8–10 weeks, but its threads and API replace `rtc.c` wholesale | (d) plus 4–6 weeks of IPC; a context switch and a copy per packet | **10–14 weeks** | GStreamer: 6–8 weeks plus a heavy tree; WPE: unknown; Pion: excluded by the brief |
| Security risk | high: new DTLS and SCTP implementations are where WebRTC stacks have had their worst bugs (usrsctp's CVEs are the precedent) | medium: libjuice is small and fuzzed upstream; libdatachannel adds C++ and its own parsers | medium-low, but the split buys little: SRTP keys still pass through the network process | **low-medium**: no new crypto or SCTP code; new code is the ICE state machine and SDP writer, parsed with `rtcwire.c`'s strict style and fuzzed | GStreamer's plugin surface is large; WPE runs a browser engine with the screen |
| Browser compatibility burden | highest: every browser quirk is ours, in our DTLS and SCTP | low for libdatachannel (used by OBS's WHIP, many clients); libjuice is RFC-correct | same as (d) | medium: ICE and SDP are ours, but those are the parts that are simple text and state, and 5673 already keeps up with Chrome, Firefox and Safari | low |
| Dependency policy | fits (no new library) but contradicts the reason OpenSSL is approved | **libjuice and libdatachannel are not in Debian 13** (`rtc.c:4-5`, `docs/ARCHITECTURE.md:106`): vendored, so LordOS owns their security updates. Each is a new library needing approval | fits (OpenSSL already approved) | **fits: OpenSSL, libsrtp2 and usrsctp are approved and packaged; each gets one more binary** | GStreamer/libnice not in the policy; WebKit is forbidden for `lord-server` and in the dependency list |
| New helper | `lord-beam-net` | `lord-beam-net` | `lord-beam-net` and `lord-beam-crypto` | `lord-beam-net` | — |
| AGENTS.md exceptions | network helper | network helper; a new vendored library | two helpers, OpenSSL in one more | network helper; OpenSSL, libsrtp2, usrsctp in one more binary each | — |

**(c), read differently,** is what LordOS does for HTTPS: `lord-tls` relays one TLS stream for a socket someone else connected. For
the **signalhub WebSocket alone** that pattern fits well, and is worth considering as a sub-option of (d): the network helper would then
need no CA bundle and no TLS client code. But DTLS is per packet and tied to SRTP key export; splitting it out costs latency and
gains nothing, since a compromised network helper already holds the session.

**Fallback inside (d):** if the full ICE agent turns out harder than estimated, libjuice is the smallest library that provides
exactly that piece, and could be vendored behind `rtc.c`'s existing DTLS/SRTP/SCTP. That would need the maintainer's approval of a
vendored dependency; it's not the first choice.

### 2.3 Recommendation

**(d).** It reuses the code that already carries LordOS's remote desktop to Chrome, Firefox and Safari, adds no library, and puts
the new work where LordOS's own rules want it: strict parsers, a fuzzer, an attack run. It also improves 5673: full ICE gives the
LAN remote desktop a path through NAT when a relay is configured, and TWCC gives it a better rate controller.

New AGENTS.md entries it needs (all for the maintainer, section 8):

1. **`lord-beam-net`**, a new sandboxed network helper (3.9): links OpenSSL (the sixth binary), libsrtp2 and usrsctp (the second
   each); may create `AF_INET`/`AF_INET6` UDP and TCP sockets, `connect`, `bind`; never `listen`, `accept`, `AF_UNIX`, netlink,
   `fork`, `exec`; no file access after start; its own scope and firewall app id.
2. **`share.c` serves one system helper** tied to the id `lordos:beam`, like a companion's fd 4, without the packaged-app root.
3. **New screen-sharing API messages** (3.5, 3.7): `SHARE_BEACON`, `SHARE_REL`, extra buttons, `SHARE_CURSOR_IN_VIDEO`, and a size
   for scaling. All general, none go-link-specific.
4. **The shared `server/src/rtc/` library** replacing `remote/rtc.c` in `lord-sessiond`'s bridge (a refactor of approved code, recorded
   so a reviewer sees 5673 changed).

## 3. Architecture

### 3.1 Processes

```
                         ┌──────────────── lord-server (the session, no OpenSSL, no FFmpeg) ─────────────────┐
  Lord Beam app (JS)     │  share.c: grant, indicator, lock, one door, input rules, protected windows        │
  lord-server --app beam │  js_beam.c: the app's API; the visitor-acceptance record; fs for files            │
  broker ───────────────►│  mstream ──memfd slots──► lord-media (VA-API / x264 / VP8, Opus; sandboxed)        │
                         │         ◄── encoded AU ───┘                                                         │
                         │  spawns ▼ fixed path, no args, fd 3 control, fd 4 share link                        │
                         └────────────────┬───────────────────────────────────────────────────────────────────┘
                                          │ SOCK_SEQPACKET, fixed-format messages, SCM_RIGHTS for file fds
                         ┌────────────────▼───────────────── lord-beam-net (sandboxed, scope lordos-net-beam) ┐
                         │ signalhub WSS client · go-link messages · full ICE/TURN · DTLS · SRTP · SCTP        │
                         │ packetize AU → RTP → SRTP → sendmmsg · data channels ↔ fixed messages to lord-server │
                         └──────────────── UDP/TCP to signal.go-link.org, its TURN, and the viewer ────────────┘
```

| Process | Does | Never does |
|---|---|---|
| **App** (`apps/beam/`, system app, isolated `lord-server --app beam`) | everything you see: home, devices, links, settings, the visitor's acceptance dialog with the scam warning, the session panel, chat, files list, the viewer window | touch the network, keys or files |
| **`lord-server`** (`js_beam.c`, `share.c`) | spawns and supervises the helper; holds the **acceptance record** (which visitor the person accepted, with which permissions), the system grant and the indicator; turns visitor input into injected events under `share.c`'s rules; opens files for transfers; makes cursor PNGs; keeps settings and the link store's files | parse SDP, JSON from the network, or video |
| **`lord-media`** | encodes the share (H.264/VP8, Opus); in a call, decodes the visitor's camera and mic and encodes this computer's camera; as viewer, decodes the remote picture | network |
| **`lord-beam-net`** | everything with a network byte in it | files, the screen, input injection, decisions about consent |

Why a helper started by `lord-server` and not a companion: the app is LordOS's own, signed with the release key, so the packaged-app
root, `BOP_COMPANION`, Verified-author checks and the companion's Landlock home rules don't apply and aren't needed. The helper
follows `lord-stream`'s and `lord-rdesk`'s shape (fixed path next to `lord-server`, no arguments, minimal environment, its own scope,
sandboxed before it reads a byte from the network), which AGENTS.md already accepts for network helpers.

Why consent is recorded in `lord-server`, not the helper: the helper is the process most exposed to hostile input. If it were
compromised it must not be able to start a share by itself. `share.c` serves frames and accepts input from the helper only while
`lord-server` holds a live acceptance that came from the app (the person pressed Accept) or from an unattended link (the one case
that depends on the helper's verification, see 5.2), plus the system grant and the indicator on screen.

### 3.2 Frames: from the composed frame to the wire

This is the same path 5673 and Desk go-link use (`docs/REMOTE.md` "One encoder per session", `docs/SCREEN-SHARING.md`), with the
helper where the bridge or the companion was.

1. The compositor composes as always. On the GPU path the frame is **read back once** (`gpu_read_next`, `main.c:2366`); with a
   protected window on screen the CPU path paints the protected fill instead. `mirror_frame` (`mirror.c:671`) gets the damage box.
2. `mstream_frame` compares only the 64×64 tiles inside the damage (all of them twice a second) and copies only what changed into
   one of three sealed memfd slots shared with `lord-media`. A still screen costs nothing and sends nothing.
3. `lord-media` encodes: VA-API H.264 where the card has it (the RX 580's VCE 3.4, first keyframe verified by `lm_encheck.c`), else
   libx264 (`ultrafast`, `zerolatency`, CRF 22 with a cap), or libvpx VP8 (`realtime`).
4. The encoded access unit goes back to `lord-server` and, through `share.c`, as `RP_VFRAME` to the helper on its share link.
5. `lord-beam-net` packetizes (`rw_h264_packetize`/`rw_vp8_packetize`), stamps TWCC sequence numbers, encrypts with SRTP and sends
   with `sendmmsg`, paced.

Raw-pixel copies today: the GPU read-back, the damaged tiles into the slot, and the encoder's upload. Encoded bytes (a few hundred
KB a second) are copied twice more on the way to the helper, which is negligible.

**What to improve (phase 5):**
- **DMA-BUF into VA-API.** On the GPU path, hand `lord-media` the composed frame as a DMA-BUF (an fd, which the policy allows) and
  import it as a VA surface: no read-back, no upload, colour conversion in VA's video processor. This is the single biggest saving
  on the RX 580 and removes most of `lord-server`'s per-frame cost. Not possible with protected windows on screen (CPU path) or on
  the Pi 5 (no H.264 encoder).
- **Encoded frames straight from `lord-media` to the helper** over a socketpair `lord-server` sets up, with `lord-server` keeping
  only the metadata: saves a hop. Small gain; only if measurements say so.
- **Scaling** (3.4) is a prerequisite for go-link's presets and for the Pi.

**Codecs.** H.264 Constrained Baseline is what `lm_stream.c` makes today (`AV_PROFILE_H264_CONSTRAINED_BASELINE`, `:100`; x264
`profile baseline`, `:137`). Constrained High (CABAC, 8×8 transform; about 10–15 % fewer bits at the same quality, estimate) is
what go-link prefers when the viewer lists `h264-high`; VA-API on the RX 580 can encode it. Offer High when the encoder is VA-API,
Baseline with x264 (`ultrafast` turns CABAC off anyway). **AV1 and VP9 are not in go-link's contract:** the knock's `codecs` lists
only `h264-high`, `h264` and `vp8`, the web viewer's knock is built from that list, and neither the RX 580 nor the Pi 5 encodes
AV1 in hardware. AV1 would need a go-link change on every client first; not proposed.

**One encoder, shared with 5673 in code, not in time.** The one-door rule means 5673 and a share never run at once in a session, so
they don't share a running encoder; they share `mstream`, `lm_stream.c`, the packetizers, the RTP/RTCP code, DTLS, SRTP and SCTP,
and after this work the ICE agent and the rate controller.

### 3.3 Congestion control and bitrate

Today 5673 adapts on loss from receiver reports and uses REMB as a ceiling (300 kbit/s–12 Mbit/s, `docs/REMOTE.md`). Browsers stop
sending REMB when `transport-cc` is negotiated, and go-link's viewers expect a sender-side estimate. Plan, in the shared library:

1. **TWCC**: stamp every RTP packet (video, RTX, audio) with the transport-wide sequence number; parse transport-cc feedback.
2. **Estimator**: a delay-based trendline (GCC's arrival-time filter and overuse detector) plus a loss-based bound, the minimum of
   the two; and go-link's own correction (`go-link:docs/architecture.md` "The picture"): count a drop only when the encoder was
   filling the estimate, cap by what was actually sent, and probe a held estimate.
3. **Pacer**: a leaky bucket at 2.5× the target, so a 100 KB keyframe doesn't hit the path as one burst.
4. **Encoder**: target 90 % of the estimate under the preset's ceiling. x264 reconfigures between frames (`lm_stream.c:289`);
   VA-API via FFmpeg keeps its rate (`:289` comment), so the hardware path needs either a context reopen on large changes (a
   keyframe, at most every few seconds) or libva's own rate-control parameter buffers, which `lord-media` may use (it links libva).
5. **The ladder** of 1.8 (fps, then size) once scaling exists; the Pi's ladder starts lower.
6. `client_report`'s loss and freezes cap the estimate, and move the playout delay (3.5).

Shared with 5673, which gets the same estimator for free.

### 3.4 Size, presets and scaling

`mstream` has no scaling (GO-LINK.md §4.4) and a resize starts a new encoder. go-link's presets and `video_want` assume the host
scales. Add a target size to `mstream_want` and scale in `lord-media` before the encoder: VA-API's video processor on the RX 580
(free), libswscale elsewhere. On the Pi 5 this is the main lever: 1280×720 costs x264 about 45 % of 1080p (pixels; estimate).

### 3.5 Latency

Budget at 60 fps on a LAN (targets): compose and read-back 3 ms; tile compare and slot copy 1 ms; VA-API encode 3–5 ms (x264
7–13 ms measured at 1280–1568 px, `docs/REMOTE.md`); packetize, encrypt, send under 1 ms; network 1 ms; browser jitter buffer
**the dominant term**: 15–80 ms measured on 5673 while dragging; decode 2.6 ms; display up to one refresh (16.7 ms).

- **Playout delay.** Send `playout-delay` min 0, max 0–30 ms on video (go-link does, `playout_delay.go`) and move it with
  `client_report` (freezes raise it, smooth playback lowers it). This is how to get from 5673's 72–130 ms (median about 90 ms,
  measured) to the target.
- **Pacing without queuing**: never let the socket's send queue hold more than one frame.
- **Measuring**: go-link's beacon (1.5) needs the host to flip a 32-point square in the top-left of the shown display, above
  everything, click-through, as part of the composed frame. Add `SHARE_BEACON {on}` to `share.c`: the compositor draws it in the
  `notification` layer of the shown display, and only while a share runs. The viewer then reports glass-to-glass latency itself.

Targets: LAN under 50 ms median on the RX 580 at 60 fps, under 80 ms on the Pi 5; through TURN, the round trip to the relay plus
60 ms.

### 3.6 Sound

`mstream` makes mono Opus at 64 kbit/s from the session's output (`docs/REMOTE.md:630` per GO-LINK.md). go-link offers stereo at
96 kbit/s. Make `lm_stream.c`'s Opus encoder stereo at 96 kbit/s (the PipeWire capture is the session's sink monitor), for both
doors. Nothing is sent before `permissions.audio` (`SHARE_HEAR`). In a call the visitor's voice is played on this computer; it must
be left out of the share's sound or the visitor hears themselves: play the call through a PipeWire stream marked so `lord-server`'s
capture skips it. That rule doesn't exist yet: **to design** with `mpulse.c`/`maudio.c`.

### 3.7 Input

All of it goes through `share.c` (`docs/SCREEN-SHARING.md`, "Input"), so these hold without new code: no input without `remote-input`
and `permissions.control`; dropped while the `overlay`, `modal` or `system` layers show something; presses aimed at protected windows
or at the Lord Beam app's own windows are dropped (a visitor can't accept themselves or tick a permission); a release always goes;
1,000 pointer events and 200 keys a second; everything released when the share ends.

New, general additions to the API: relative moves (`0x02`, pointer lock), back and forward buttons, and `SHARE_TEXT` already
covers `0x31`. The helper only validates the packets (fixed sizes, display index, known key index) and forwards fixed-size
messages; `share.c` validates them again.

### 3.8 Clipboard and files under LordOS's privacy rules

**Clipboard** (`share.c`'s rule): text only, only with `permissions.clipboard` and the `clipboard` flag, only what is copied after
the start; **items marked sensitive never go** (Passwords' mark, `term_clipboard_sensitive`). A visitor's `paste` sets the clipboard
and, only with `control`, presses the layout's paste key; text fields marked as password fields get the text like any typed text
(the visitor is in control) but nothing reads a password field back. Clipboard text is never logged.

**Files**: the helper has no file access at all. `lord-server` does the file work with the same rules as `lord.fs` and hands
descriptors over `SCM_RIGHTS`:

- **Browse**: `lord-server` lists the home folder's *visible* entries, never a dot entry, never through a link out, and never
  anything `fsops_protected` covers (`.desktop` folders, autostart, `settings.json`, `permisos.json`, `firewall.json`, the vault, the
  blocklists, …). The protected list is the same one `lord.fs` writes against; for reads the rule is "visible home entries minus
  the protected ones".
- **Download**: `lord-server` opens the file read-only (`O_NOFOLLOW`, re-checked after open) and passes the fd; the helper reads at
  the offsets asked.
- **Upload**: `lord-server` makes an unnamed temporary file in `~/Downloads` (`O_TMPFILE`) and passes the fd; on `end` it links it
  under a cleaned, non-colliding name; on `abort` or a dropped session it's gone by itself.
- `FW_R_NO_FILES` refuses files; every transfer is logged with the visitor's id, name and size.

**Protected windows** stay as the protected fill in the picture (the API takes the frame after `media_composed`), take no input,
and a screenshot or recording by the viewer contains the fill.

### 3.9 The helper's sandbox

`sandbox_apply_helper()` with `network = 1` as `lord-stream` uses it, plus what DTLS and TLS need loaded before:

| Limit | Value |
|---|---|
| Start | fixed path `<lord-server dir>/lord-beam-net`, no arguments, environment `LANG` only, fd 3 the control link, fd 4 the share link, fds 0–2 `/dev/null`; refuses to run as root |
| Before the sandbox | loads the CA bundle (for the signalhub WSS and a `turns:` relay); reads nothing else |
| Identity | no capabilities, `no_new_privs`, scope `lordos-net-beam.scope` (firewall app `lordos:beam`), memory 256 MB, one core weight |
| Sockets | `socket` only `AF_INET`/`AF_INET6`, `SOCK_DGRAM` and `SOCK_STREAM`; `connect`, `bind` (UDP, for ICE), `sendmmsg`/`recvmmsg`, `setsockopt` for TOS/DSCP and buffers; **never** `listen`, `accept`, `AF_UNIX`, `socketpair`, netlink |
| Interfaces | no netlink: `lord-server` sends the list of local addresses and a message on each change, which is also what triggers an ICE restart |
| DNS | Landlock read-only on `/etc/resolv.conf`, `/run/systemd/resolve`, `/etc/hosts`, `/etc/nsswitch.conf` (as `lord-stream`); or names resolved by `lord-server` and passed as addresses (smaller; to decide) |
| Files | none after start (Landlock with no write rule, no read rule outside the above) |
| Process | no `fork`, `clone` except threads (none planned: usrsctp runs threadless, `rtc.c`), no `execve`, no `ptrace`, no `memfd_create` |
| Firewall | the person's firewall rules for `lordos:beam` apply; a "block" on it ends sessions |

The helper never sees files, the screen or the keyboard; what it can leak if compromised is what the share grants at that moment
and the link store's public keys and device key (5.2).

### 3.10 Multi-monitor

`SHARE_DISPLAYS` gives the list (connector ids, logical and pixel sizes, main, the "all" picture `0xffffffff`); the helper turns it
into go-link's `displays` (`points_width`/`points_height` from the logical size). `display_select` → `SHARE_DISPLAY`; LordOS answers
with the list, which has the last word. Pointer positions are 0..65535 over the shown picture, mapped back by `share.c`. The share
starts on Settings → Displays → "Screenshots and sharing" (main display by default). As a viewer, the app shows the remote
`displays` in the toolbar's display menu.

### 3.11 Consent and the capture indicator

Two consents, as in GO-LINK.md §1, kept apart:

1. **The system's** (once per app): `screen-share` and `remote-input`, drawn by LordOS, stored root-owned by `share-grant`. For a
   LordOS system app the maintainer may decide to skip the first-use prompt (section 8); the indicator stays either way.
2. **The visitor's** (each session): drawn by the app. The request shows the declared name and browser (marked as declared), the
   permissions as boxes, the scam warning the first time a code is shown, 60 s; Settings → "A request starts with: Only the screen"
   as in go-link.

While a session lasts: the capture indicator by the clock (`sdk/capturestatus.jsx`) with **Stop sharing**, frames only after it is
on screen, ⌃⌥⇧Esc ends it from C, the lock and user switch end it in `share.c`, and the app's session panel (who, since when,
permissions changeable live, chat, Disconnect) stays on screen and takes no visitor input.

### 3.12 The other direction: connecting to another computer

The app is also a viewer, as every go-link client is. The helper answers instead of offering, sends a knock with `codecs:
["h264-high","h264","vp8"]` (everything `lord-media` decodes; VA-API decode on the RX 580), passes the received RTP, reassembled into
access units, to a `lord-media` live decoder (`media.live`, PACKAGED-APPS.md "Live media"), and the app shows it in a `MediaView`.
Input goes the other way as go-link's packets, from LordOS key events to the `KeyboardEvent.code` index. As a viewer the remote
picture is untrusted video decoded only in `lord-media`'s sandbox. A host's `cursor` PNG is decoded in `lord-media` too, never
in `lord-server`.

## 4. The interface, modelled on Machines

Machines' kit (`apps/machines/kit.jsx`, `ui.jsx`): a sidebar of `SideRow`s with `StateDot`s, `PaneHeader`s, grouped `Section` cards,
`Pill` buttons, `Sheet`s that slide over the window instead of pages, `ToolPill`/`ToolGlyph` toolbars, `usePal()` for light and dark,
glyphs from `assets/machines/symbols`. Lord Beam uses the same kit (moved to the SDK or copied as its own kit; not imported from
`apps/machines`, since apps import only `@lordos/sdk`). Text through `t('beam.*')` in en, es and pt; `textAlign: 'start'/'end'`; the
code field uses `direction: 'ltr'`.

| Screen | Content |
|---|---|
| **Sidebar** | *This computer* (dot: green ready, blue in session, grey offline), *Connect*, *My devices* (linked computers, favourites first), *Recent*, *Links* (browsers linked to this computer), *Settings* at the bottom |
| **This computer** | a large card: the 9-digit code `915 355 636` in a monospaced face, the link `desk.go-link.org/c/<invite>` with Copy and a QR code, **New code**, status ("Ready", "Connecting to the network", "Offline: retrying"), the system grant's state with a button to the privacy page |
| **Connect** | one field taking a code, an invite link, or a `/d/` link; recent targets below as cards with thumbnails, as Machines' library |
| **My devices / Recent** | cards or a list (Machines' library toggle), each with the thumbnail of the last session, name, OS mark, last use, favourite star, menu (Rename, Forget) |
| **Links** | the browsers linked for unattended access: declared name, linked on, last use, permissions as toggles, **Revoke** (`Pill danger`) |
| **Settings** (a `Sheet` with sections, like Machines' settings window) | *Sessions*: a request starts with all / only the screen; end after idle 5 min–1 h; lock when a session ends. *Unattended access*: off by default, password (twice), pairing code shown here only. *Network*: signaling server (wss only, warning for others), relay only, "Test my connection" (opens desk.go-link.org/network with the `#d=` data of `go-link:docs/network.md`). *Video*: default preset. *About* |
| **Incoming request** | a modal `Sheet` in the app *and* a system notification: name and browser "as they say", permission boxes, the first-time scam warning, Reject / Accept, a 60 s ring |
| **Session panel** | a small always-on-top window: who, since when, Direct/Relay, permission toggles, chat with typing, files in flight, the call, Disconnect. It ignores visitor input (3.7) |
| **Viewer** | a window per remote computer like Machines' VM window (`vmwindow.jsx`): the picture in a `MediaView`, a `ToolPill` bar with display menu, preset, fit/1:1, keyboard capture, send Ctrl+Alt+Del, clipboard, files, chat, call, screenshot/record (asks the host), stats (codec, fps, kbps, path, latency from the beacon), Disconnect |
| **Call** | a floating window: the remote camera, this camera's preview, mute, camera off, hang up |
| **About** | the app's name and LordOS version; "Uses the go-link Desk network" with the attribution text of section 8; links to go-link Desk's privacy page and to LordOS's |

Status item by the clock (`statusItem`), red during a session: code, Disconnect, Open, Settings.

## 5. Security

### 5.1 Threat model

| Threat | What stands against it | What remains |
|---|---|---|
| **Hostile visitor** (scammer, curious, malicious viewer) | nothing before acceptance (no offer, nothing gated passes); name and UA sanitized and shown as declared; 60 s; one knock per join; per-permission boxes, live toggles; input through `share.c`'s drop rules (system surfaces, protected windows, the app's own windows); clipboard after-start and not-sensitive; files only visible home entries minus `fsops_protected`, writes only new files in Downloads; record/screenshot asked; indicator and Stop; idle end; lock end | they can do what the person can do in ordinary windows with control; a dishonest viewer can record what they see |
| **Hostile signalhub** | it never sees media (DTLS-SRTP end to end); it can't start a session without acceptance; linked devices are bound by the ECDSA proofs over both fingerprints; `meta` holds only the short name and OS | it sees who connects and when, the codes; for a *code* session a signalhub controlling both sides can sit in the middle of a session the person accepted (go-link's residual risk; the short verification code of GO-LINK.md D5 is the fix, to be defined on go-link's side) |
| **Hostile TURN** | sees only DTLS/SRTP; credentials are short-lived | size and timing |
| **Hostile network** | every packet parsed by strict fixed-size parsers (`rtcwire.c` style) and fuzzed; ICE checks need the credentials; media moves to a new path only after an authenticated packet from it (5673's rule); DTLS cipher list limited; fingerprint check | denial of service |
| **Bug in the helper reached from the network** | the sandbox of 3.9: no files, no exec, no `AF_UNIX`, no screen or keyboard of its own; `lord-server` re-validates every message from it; consent recorded in `lord-server` | while a session runs, what that session grants; the device's private key and link public keys (5.2) |
| **Local processes** | the share socket is not used (the helper's link is made by `lord-server`); the app's data (device key, links, settings) lives in `~/.local/share/lordos/appdata/beam/`, which goes into `fsops_protected` so `lord.fs` apps can't add a link; isolated apps can't reach the helper | an unsandboxed process of the same user can still edit the user's files, as for every app; the device key could be kept in the keyring instead (section 8) |
| **Visitor's video during a call, or a remote host's picture** | decoded only in `lord-media`'s sandbox; the helper forwards bytes | a decoder bug inside `lord-media`'s sandbox |
| **Lock and user switch** | `share.c` ends the share synchronously before the next frame | a frame already sent |
| **Forged app or helper** | both are LordOS's own, in `code-manifest.json`, signed with the release key; the helper at a fixed root-owned path | the release key |

### 5.2 Unattended access

It is the only path where the person doesn't press Accept, and it depends on the helper's ECDSA verification (OpenSSL lives there,
not in `lord-server`; Monocypher has no P-256). A compromised helper could therefore claim a link verified. Mitigations: off by
default, and behind a password kept as an argon2id hash (OpenSSL 3.5's KDF, so no new library). `lord-server` has neither P-256
nor argon2id today, so both checks run in the helper. **Open point:** either accept that the helper's integrity guards unattended
access, as in go-link, or move the proof and password checks into a tiny second helper with no network. The device's private key is the
other asset: in a file 0600 in the app's data folder (as go-link's `device.json`), or in LordOS's keyring so the helper only gets a
signing service. Section 8 asks.

### 5.3 What the helper parses

signalhub JSON (64 KB cap), `signal.payload` (knock, SDP, candidates), STUN/TURN, DTLS (OpenSSL), SRTP/SRTCP (libsrtp2), SCTP
(usrsctp), DCEP, RTCP including transport-cc, control JSON (64 KiB), input packets, files messages, received RTP (calls, viewer).
Each new parser gets a case in a `beam-sec-test` like `remote-sec-test` and a fuzz target like `remote-fuzz`, run in
`make -C server test`.

## 6. Performance targets and how to measure them

### 6.1 What's known

**Measured** (GO-LINK.md §6, REMOTE.md): x264 7–17 ms a frame (1280×800 still to 1920×1080 moving); VM 125 share: `lord-server`
6.5 %, engine 3.7 %, `lord-media` 1.0 % of a core on a still screen; Pi 5 share at 1080p with motion: `lord-server` 97 %
(read-back bound), `lord-media` 52 %, about 21 fps at the visitor; 5673 latency 72–130 ms, median about 90 ms, jitter buffer
15–80 ms. **Not measured:** VA-API on the RX 580 in a share (the encoder check passes there, `docs/REMOTE.md`).

### 6.2 Targets

| | RX 580 PC (VA-API) | Raspberry Pi 5 (x264, GPU composition) |
|---|---|---|
| Balanced | 1920×1080, 60 fps, H.264 High, ≤ 10 Mbit/s | 1280×720, 60 fps or 1920×1080, 30 fps, Baseline, ≤ 6 Mbit/s |
| Quality | 2560×1440 at 60 | not offered above 1080p30 |
| Glass-to-glass, LAN, median (beacon) | ≤ 50 ms | ≤ 80 ms |
| CPU, everything LordOS adds, moving picture | ≤ 25 % of one core (≤ 10 % with DMA-BUF import) | `lord-server` ≤ 60 %, `lord-media` ≤ 70 % |
| CPU, still screen | ≤ 2 % | ≤ 5 % |
| `lord-beam-net` | ≤ 5 % of a core at 10 Mbit/s, ≤ 40 MB RSS | ≤ 10 %, ≤ 40 MB |
| Time to picture after Accept | ≤ 1.5 s direct, ≤ 3 s relayed | same |
| Recovery from a 5 s path loss | picture back ≤ 1 s after the path returns | same |

### 6.3 How to measure

- **Latency**: the viewer's own beacon (1.5; needs `SHARE_BEACON`), median of 7 flips, from the web viewer and from an iPad.
- **Frames and bitrate**: `stream_stats` and the viewer's statistics panel; `chrome://webrtc-internals` for jitter buffer, freezes,
  NACKs, the answer's negotiated extensions.
- **CPU and memory**: per process from `/proc` over a scripted 60 s drag and a 60 fps video window, as in REMOTE.md's tables;
  `mstream_info` for encode time; the no-diag build used for the compositor benchmark.
- **Encoder alone**: `lord-media`'s bench (`lm_vbench.c`, LordBench's clip) at 1080p and 1440p, VA-API and x264.
- **Network conditions**: `tc netem` on the VM's link (delay 20/80 ms, loss 1/5 %, rate 2/5/10 Mbit/s); check the ladder steps
  and recovery, and the relay path with `iceTransportPolicy: "relay"` on the viewer.
- **Where**: the RX 580 PC (the maintainer's main machine, real monitors, VA-API), the Pi 5 (monitor, GPU composition), VM 127 for
  end-to-end on an installed LordOS. Every milestone with a real screenshot.

### 6.4 Interoperability checks (each release)

Against the public signalhub: the web viewer in Chromium, Firefox and Safari; the iOS/iPadOS app; the Android app; a go-link Desk
desktop host (Lord Beam as viewer) on macOS and Linux. Code session, invite link, linked device with password, revoke, display
switch, clipboard both ways, upload and download, call both ways, ICE restart by cutting the path, relay only. go-link's
`protocol/vectors/*.json` in the unit tests.

## 7. Phases, effort and risks

| Phase | Work | Effort (estimate) | Ships as |
|---|---|---|---|
| **0. Library** | `remote/rtc.c` → `server/src/rtc/`, no behaviour change; `remote-sec-test`, `remote-fuzz`, 5673 e2e green | 2 weeks | nothing visible |
| **1. Host for the web viewer** | signalhub WSS client; register, invite room, code and link; knock/accept/reject; SDP offer writer; full ICE with STUN and TURN REST; DCEP opener; `control`/`pointer`/`input`; H.264 CB and VP8; Opus; `share.c` link for the helper and the acceptance record; app: This computer, request, session panel, Settings basics, About | 10–12 weeks | **first shippable**: share this computer with desk.go-link.org, "in development" |
| **2. Parity of a session** | clipboard, files (lord-server fd plumbing), chat/typing, displays, cursor messages, stats, ping, ICE restarts, idle, TWCC + estimator + pacer, scaling and presets, stereo Opus, playout delay, beacon; phones verified | 6–8 weeks | sessions with the phones and the web at parity |
| **3. Links and viewer** | unattended access (device key, claim, reach, proofs, password, revoke); the viewer side (answerer, RTP receive, `lord-media` live, input out); My devices, Recent, Links | 6–8 weeks | full app |
| **4. Calls and captures** | call tracks both ways (camera via `lord-media`, mic, call audio excluded from the share's sound), screenshot/recording consent | 3–4 weeks | |
| **5. Speed and review** | H.264 High on VA-API, DMA-BUF import, VA-API rate control, measurements of section 6, security review and an attack run like REMOTE.md's, fuzzing hours | 4–6 weeks | the release |

Total about 31–40 weeks for one developer, 5 to 7 months with review and VM time; phase 1 alone is about 3 months including phase 0.

**Risks**

- **Full ICE in our own code** is the largest unknown (nomination, aggressive vs regular, triggered checks, TCP to TURN, IPv6,
  mDNS candidates from browsers which a host can't resolve and must ignore or answer through peer-reflexive). Mitigation: the
  libjuice fallback of 2.2.
- **Contract drift**: go-link changes a message or the SDP. Mitigation: vectors, the per-release check of 6.4, and the maintainer
  owning both.
- **Phones' WebRTC stacks** may be stricter than Chrome about the offer (section 1.9).
- **VA-API rate control** may force encoder reopens; fallback is x264 at a lower size.
- **Pi 5 read-back** bounds the frame rate before x264 does (measured); only scaling helps there.
- **Two remote-desktop apps on LordOS** (5673, Lord Beam, and Remote Desk for RDP/VNC) confuse people; naming and the Store text must
  say which is which.

## 8. Decisions for the maintainer

1. **Name** (each with app id, helper and package):
   - **Lord Beam**: id `beam`, helper `lord-beam-net`, ships in the `lordos` package; if a separate optional package is wanted,
     `lordos-beam`. Short, says "send your screen", no clash with Remote Desk or 5673. *Recommended.*
   - **Lord Reach**: id `reach`, `lord-reach-net`, `lordos-reach`. Clear, but "reach" is also a signalhub message, which makes the
     code and the docs ambiguous.
   - **LordOS Assist**: id `assist`, `lord-assist-net`, `lordos-assist`. Says what it's mostly for (helping someone), weaker for
     reaching one's own computer.
   - Package: an exclusive system app normally ships inside `lordos` (it's in `code-manifest.json` and signed with the release key).
     A separate `lordos-beam` package needs the code manifest to accept an optional signed bundle: decide which.
2. **Stack**: option (d), grow `rtc.c` into `server/src/rtc/` with a full ICE agent; libjuice only as an approved fallback if ICE
   slips. And whether the signalhub WSS goes through `lord-tls` (2.2) or the helper's own TLS.
3. **New helper and exceptions** to record in AGENTS.md: `lord-beam-net` (network sandbox of 3.9), OpenSSL/libsrtp2/usrsctp in it,
   `share.c` serving it as `lordos:beam`, the new API messages (`SHARE_BEACON`, `SHARE_REL`, buttons 4–5, pointer-in-video, scaled
   size), file descriptors for transfers opened by `lord-server`, the app's data folder added to `fsops_protected`.
4. **The system prompt** for a LordOS system app: keep the one-time `screen-share`/`remote-input` prompt (consistent with every other
   app) or pre-grant it (the visitor's acceptance and the indicator remain). Recommended: keep it.
5. **Unattended access**: accept that the helper's integrity guards it (as in go-link), or add a small verifier; device key in a file
   or in the keyring (5.2).
6. **Branding and attribution** (About, Store, site; en/es/pt): "Lord Beam uses the go-link Desk network: its signaling server and
   relays at go-link.org, and it works with desk.go-link.org and the go-link Desk apps. go-link Desk is a separate project (MIT).
   Lord Beam is LordOS's own implementation and shares no code with it." Plus a link to go-link Desk's privacy page, since its servers
   see who connects. Confirm the go-link name may be used this way, and reading go-link's `keycodes.json` and test vectors in LordOS's
   tests (data, not code).
7. **The existing port** (Desk go-link: the React app plus the sandboxed Go/Pion companion, `go-link-desk-lordos`, Store → Remote
   access): recommended, keep it as is until Lord Beam reaches phase 3, then stop recommending it in the Store, keep it installable as
   a third-party app, and keep the screen-sharing API and packaged-app/companion support, which are general. Offer an import of its
   `device.json` (device id, secret, device key, links) so linked browsers and phones keep working without relinking.
8. **AV1**: not now; it would need go-link's clients to add it to the knock first.
