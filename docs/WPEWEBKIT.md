# WPE WebKit for Debian 13: LordOS's backport

LordOS builds its own WPE WebKit for Debian 13 (trixie) and publishes it in the LordOS APT repository, next to its
exact source. Maintainer's decision of 2026-11-23, for [#142](https://github.com/lordbasex/LordOS/issues/142)
(ROADMAP, "Before 1.0": lord-web on the new WPE API). In development: the packages are built and tested, not yet
published; the latest release, 0.8.9.5-beta, still uses Debian's 2.48. The current build is `2.54.3-1~lordos13+2`
(WPEPlatform's DRM backend off, maintainer's decision of 2026-11-24; `+1`, with it on, was never published).

## Why

- `lord-web` ([WEBAPPS.md](WEBAPPS.md)) is the only LordOS binary that links WPE WebKit. Today it uses WebKit's legacy
  API (`libwpe` and `wpebackend-fdo`), which WebKit has deprecated in favour of **WPEPlatform** (`wpe-platform-2.0`).
- Debian 13 ships `wpewebkit` 2.48 built **without** WPEPlatform (no `wpe-platform-2.0.pc`), so lord-web cannot move
  to the new API with trixie's packages. Debian unstable/forky has 2.54 with WPEPlatform.
- The backport is Debian's own source package of the newer version, rebuilt on trixie with as few changes as possible,
  so it keeps Debian's package split, names and SONAME and replaces trixie's 2.48 as an ordinary upgrade. It turns
  both APIs on: WPEPlatform (its headless and Wayland backends) for lord-web's WPEPlatform backend, and the legacy API
  so lord-web's legacy backend, and anything else built against 2.48, keeps working.

## What gets built

`tools/pkg/build-wpewebkit.sh <debian-version> [lordos-revision]`, for example `tools/pkg/build-wpewebkit.sh 2.54.3-1 2`,
makes version `2.54.3-1~lordos13+2` (`~` sorts it below Debian's own `2.54.3-1`, so a later Debian package of the same
version wins; `lordos13` is Debian 13). `ARCH=arm64` builds arm64 on an amd64 machine under qemu-user (below). Its
steps, each in a clean chroot made by `mmdebstrap` (a Debian build tool, installed on the build machine with
`apt-get install mmdebstrap`; no container runtime):

1. **Fetch** Debian's source package (`.dsc`, `.orig.tar.xz` with its `.asc`, `.debian.tar.xz`) from deb.debian.org's
   pool, or from snapshot.debian.org once the pool has moved to a newer version, and wpewebkit.org's signature of the
   release tarball. Nothing downloaded is trusted until step 2 checks it.
2. **Verify and build** in a fresh `trixie` chroot (`--variant=buildd`, reading trixie, trixie-updates and
   trixie-security only):
   - `dscverify` with `debian-keyring`: the `.dsc` is signed by a Debian Developer (for 2.54.3-1, Alberto Garcia's key
     `62BC2E81004ACE51CC6058B3000C8462ED02D802`) and every file matches its checksums.
   - The release tarball's own signature verifies with the WebKit release keys that Debian ships in
     `debian/upstream/signing-key.asc`, and the signing key must be one of the two pinned in
     `tools/pkg/wpewebkit-chroot.sh` (Adrián Pérez de Castro `5AA3BC334FD7E3369E7C77B291C559DBE4C9123B`, Carlos Garcia
     Campos `013A0127AC9C65B34FFA62526C1009B693975393`). wpewebkit.org's `.asc` must be the same file as Debian's, so
     Debian's `.orig.tar.xz` is the very tarball WebKit released.
   - LordOS's changes (below), the build dependencies from trixie with `mk-build-deps`, then
     `dpkg-buildpackage -sa` as an unprivileged user: the source package and every binary package. `lintian` runs on
     the result.
3. **Test** in a second fresh trixie chroot (`--variant=apt`, trixie's packages and the new `.deb`s only): install
   them; `pkg-config` finds `wpe-webkit-2.0`, `wpe-platform-2.0`, `wpe-platform-headless-2.0`,
   `wpe-platform-wayland-2.0`, and the legacy `wpe-1.0` and `wpebackend-fdo-1.0`, and does not find
   `wpe-platform-drm-2.0`; `readelf -d` shows no `libinput` or `libudev` among `libWPEWebKit-2.0.so.1`'s `NEEDED` (the
   test fails otherwise); the SONAME and the exported symbols
   are compared with trixie's own `libwpewebkit-2.0-1` 2.48; each package has its `/usr/share/doc/<package>/copyright`;
   and `tools/pkg/wpewebkit-headless-test.c`, compiled there, opens a `WebKitWebView` on WPEPlatform's headless
   display, loads a page of a known colour and reads the frame WebKit hands to the view (`buffer-rendered`) back to
   pixels, with WebKit's web process in its bubblewrap sandbox as on a desktop. The kernel refuses user namespaces
   inside a chroot, so this stage runs as the root of its own mount namespace (`unshare` plus
   `tools/pkg/wpewebkit-pivot.sh`, which does `pivot_root` into the chroot with a fresh `/proc`, `/sys` and a minimal
   `/dev`: no GPU, so the frame is WebKit's CPU path, a `WPEBufferSHM`). `TEST_ONLY=1` repeats this stage alone.
4. **lord-web** (when the script runs from a LordOS tree, or `LORDOS_SRC=<tree>`; `LORDOS_SRC=none` skips it): in the
   same test chroot, lord-web is built on **both** of its view backends (`LORD_WEB_BACKEND=platform`, then `legacy`)
   with `-Wall -Wextra` and must have no warnings; each one then passes its own headless backend test
   (`server/tests/web_backend_test.c`: a page rendered and its pixels checked, a click, a key and the wheel reaching the
   page, resize and scale, hiding, and on WPEPlatform the cursor) and its probe test (`server/tests/web_probe_test.py`),
   both with WebKit's sandbox on.

Results go to disk as they are made: `out/` is bind-mounted into the build chroot (everything runs at `nice 19`, `ionice` idle: the build machine is a desktop in use), the packages are
synced to disk before lintian runs, and the `.changes` is written last. With `TMPFS` a power cut loses only the build
tree: running the same command again starts the build over (nothing half-done is kept), or, when a finished build
is in `out/` (its `.changes` and lintian summary), goes straight to the test (`REBUILD=1` builds again).

Results of `+2` on amd64 (2026-11-24; `out/test/report.txt`): every `pkg-config` module above found, no
`wpe-platform-drm-2.0`, and `libWPEWebKit-2.0.so.1`'s `NEEDED` has no `libinput` or `libudev` (`+1` had both); the
headless render passes with the sandbox on; lord-web (the tree at `test/integration-1125`, 0.8.9.5) builds on both
backends with no warnings, `web-backend-test` prints `ok platform (WPE WebKit 2.54.3)` and `ok legacy (WPE WebKit
2.54.3)`, and the probe test passes on both (peak 298 MB and 296 MB PSS for lord-web and its WebKit processes). On
`+1`, `tools/release/image-sources.sh` run on the test chroot listed `wpewebkit` with origin `lordos` (176 other source
packages `debian`). Not checked yet: lord-web's windows on the VM with these packages.

Results of `+3` on amd64 (2026-11-25; WebRTC and the SharedArrayBuffer patch, `out/test/report.txt` and
`out/test/webcalls.txt`): every check of `+2` above passes again (pkg-config, no DRM backend, no `libinput`/`libudev`,
SONAME, 930 of 930 symbols kept, 1345 exported, the headless render, lord-web on both backends); the library links
`libgstwebrtc-1.0`; lintian 0 errors, 5 warnings. `tests/webcalls_check.py` (in the test chroot, sandbox on): Chrome's
Client Hints and `navigator.userAgentData` equal Chrome 158's; SharedArrayBuffer is a function on a page served with
COOP and COEP and undefined on a plain page, before and after it; `RTCPeerConnection` exists; the loopback call gathers
no ICE candidate because the web process has no network (WEBAPPS.md, "Calls"). The build was interrupted by a desktop
session restart at step 8428 of 8967 and finished in the same tmpfs tree (`dpkg-buildpackage -b -nc`, then
`dpkg-genchanges -sa`); the source package was made before the interruption. arm64: not built yet; the same command
with `ARCH=arm64` (below), then the test stage on the Pi 5.

Results of `+4` on amd64 (2026-11-26; librice `0.4.3-1~lordos13+1` from `EXTRA_DEBS`): `dpkg-buildpackage` 1730 s
(16 jobs); lintian 0 errors; `libWPEWebKit-2.0.so.1` links `librice-proto` and `librice-io`; every check of `+3` passes,
and `tests/webcalls_check.py --webrtc` now connects the loopback call: host candidates (4 UDP, 8 TCP) gathered by the
network process, connected in 300 to 400 ms, Opus with VP8 and Opus with H.264, about 215 audio packets and 63 frames
each in 4 s; TURN URLs with `?transport=` accepted. Against Chrome and a public meeting server: WEBAPPS.md, "Calls".
Not published; arm64 not built yet.

Output, in `$LORDOS_WPE_WORK/<version>/out/` (default `/var/tmp/lordos-wpewebkit`; on the maintainer's build machine
`/mnt/HD08TB/build/wpewebkit`): the `.deb`s, the **source package** (`wpewebkit_<version>.dsc`,
`wpewebkit_<upstream>.orig.tar.xz`, `wpewebkit_<upstream>.orig.tar.xz.asc`, `wpewebkit_<version>.debian.tar.xz`),
`.changes`, `.buildinfo`, `SHA256SUMS`, `build-time.txt`, `lintian.txt` and `lintian-summary.txt`, and `test/` (the
test report and the rendered frame). Nothing is signed or published by the script.

### LordOS's changes to Debian's packaging

All in `backport()` of `tools/pkg/wpewebkit-chroot.sh`; a change that stops applying stops the build and says so.

| Change | Why |
|---|---|
| `debian/rules`: `-DENABLE_WPE_PLATFORM=ON`, `-DENABLE_WPE_PLATFORM_HEADLESS=ON`, `-DENABLE_WPE_PLATFORM_WAYLAND=ON`, `-DENABLE_WPE_LEGACY_API=ON` | Upstream's defaults in 2.54 already, but written out: a release that changes a default must fail here, not ship without WPEPlatform or without the legacy API |
| `debian/rules`: `-DENABLE_WPE_PLATFORM_DRM=OFF`; `debian/libwpewebkit-2.0-1.symbols`: the DRM backend's 9 symbols removed (`wpe_display_drm_*`, `wpe_{screen,toplevel,view}_drm_get_type`) | Maintainer's decision, 2026-11-24: WPEPlatform's DRM backend (WebKit driving a screen itself) is the only part that links `libinput` and `libudev`, which LordOS's dependency policy keeps out; lord-web has its own `WPEDisplay` on the compositor and never uses it. Without it those symbols aren't in the library, and `dpkg-gensymbols` stops the build unless the symbols file says so. WPEPlatform's core DRM device API (`wpe_drm_device_*`, for dmabuf) stays |
| `debian/rules`: `-DENABLE_MEDIA_STREAM=ON`, `-DENABLE_WEB_RTC=ON`, `-DUSE_GSTREAMER_WEBRTC=ON`, `-DUSE_LIBRICE=OFF`; `debian/control`: Build-Depends `libssl-dev`, and `libwpewebkit-2.0-1` Recommends `gstreamer1.0-nice`, `gstreamer1.0-plugins-bad`, `-good`, `-base` (`+3`) | Maintainer's decision, 2026-11-25: voice and video calls in web apps ([WEBAPPS.md](WEBAPPS.md), "Calls"). WebKit's GStreamer WebRTC backend (GStreamer 1.26's `webrtcbin`, not Google's libwebrtc); its only new build dependency is OpenSSL 3 (WebCore links `libcrypto` for SFrame); ICE (libnice), DTLS, SRTP, SCTP, RTP, Opus, VP8 and H.264 (OpenH264) are GStreamer elements loaded at run time. librice, the ICE agent WebKit can run in its network process, isn't in Debian 13, so it stays off: ICE runs in the web process (see "WebRTC and the web process's network" below) |
| `debian/rules`: `-DUSE_LIBRICE=ON`; `debian/control`: Build-Depends `librice-dev (>= 0.4.3)` (`+4`) | ICE in WebKit's network process ("librice" below): the web process keeps no network, and calls connect |
| `debian/patches/lordos-turn-transport-query.patch` (`+4`) | WebKit's `RTCPeerConnection` refused every TURN URL with a query (an inverted check: `query()` has no `?`, so `find("?transport=")` is `notFound`, which is true); sites hand out `turn:host:443?transport=tcp`. Now exactly `transport=udp` and `transport=tcp` are accepted, which the ICE agent reads; another query is still a `SyntaxError` |
| `debian/patches/lordos-sharedarraybuffer-coi.patch` (`+3`) | SharedArrayBuffer only in a web process made for cross-origin-isolated documents (COOP `same-origin` + COEP `require-corp`): WebKit already gives such a document a process of its own (`CrossOriginMode::Isolated`, `WebProcessProxy::shouldEnableSharedArrayBuffer()`), but only its Cocoa launcher turned SharedArrayBuffer on there; the patch does the same in the GLib launcher (`JSC_useSharedArrayBuffer=1` in that process's environment only, removed from every other's). Before it WPE never had SharedArrayBuffer, and a messaging web app requires it for calls. Checked by `tests/webcalls_check.py`: a plain page never has it, before or after an isolated one in the same view |
| `debian/control`: `Maintainer: LordOS`, `Uploaders` removed | Bugs in this build come to LordOS, not to Debian. Debian's WebKit team stays credited in the changelog and `debian/copyright` (not as `XSBC-Original-Maintainer`: Debian's lintian doesn't know that field) |
| `debian/changelog`: entry `<version>~lordos13+<n>` for `trixie`, dated one second after the Debian entry it follows | The backport's version, below Debian's own. A fixed date (later than Debian's, as lintian requires) is the build's `SOURCE_DATE_EPOCH`, so building the same version again gives the same `.debian.tar.xz` |

Nothing else changes: every build dependency of 2.54.3-1 is in trixie (clang 19 instead of sid's newer default
`clang`, libsoup 3, GLib 2.84, GStreamer 1.26, libjxl, libavif, libwebp, HarfBuzz, libgbm, libwayland,
wayland-protocols; `libinput-dev` stays a build dependency, unused without the DRM backend), and no feature had to be
turned off for trixie: apart from the DRM backend, with the same build dependencies and cmake options as Debian unstable, the features are unstable's
(no Manette gamepads, no WebRTC, no encrypted media, as there; WebRTC is on since `+3`, above). WOFF2 web fonts come from FreeType now (Debian dropped
`libwoff-dev` in 2.54.0-1: trixie's FreeType 2.13, built with Brotli, decodes WOFF2 itself). The `clang-22 [armhf]` build dependency can't be met on
trixie, but LordOS doesn't build armhf.

### The packages

Debian's split, unchanged: `libwpewebkit-2.0-1` (the library, WebKit's processes, MiniBrowser), `libwpewebkit-2.0-dev`
(headers, `.pc` files, GObject introspection `.gir`/`.typelib`), `wpewebkit-webdriver` and `libwpewebkit-doc`
(architecture `all`). There is **no new binary package for WPEPlatform**: upstream builds it into
`libWPEWebKit-2.0.so.1` itself, and its headers and `wpe-platform-2.0.pc`, `wpe-platform-headless-2.0.pc` and
`wpe-platform-wayland-2.0.pc` are in `libwpewebkit-2.0-dev` (no `wpe-platform-drm-2.0.pc`: the DRM backend isn't built). The build also makes
`-dbgsym` packages (debug symbols, about 140 MB for the library); they stay in `out/` and don't need to be published.

### ABI and replacing trixie's 2.48

Measured by the test against trixie's `libwpewebkit-2.0-1` 2.48.3-1 (`+2`, 2026-11-24):

- **Same SONAME**, `libWPEWebKit-2.0.so.1` (trixie's file is `.so.1.5.8`, the backport's `.so.1.11.6`): the same
  package name, `libwpewebkit-2.0-1`, upgrades in place.
- **No exported symbol removed**: 2.48 exports 930 dynamic symbols, the backport 1345, every one of the 930 among them
  (415 added, WPEPlatform's `wpe_*` API among them). Programs built against 2.48 (today's `lord-web`, `cog`) keep
  linking; Debian's `debian/libwpewebkit-2.0-1.symbols` for 2.54.3 is checked by `dpkg-gensymbols` during the build too.
- The legacy API stays: `wpe-1.0` (libwpe 1.16.2) and `wpebackend-fdo-1.0` (1.16.0) are trixie's own packages and
  still resolve next to the new `.pc` files.
- What else in trixie depends on `libwpewebkit-2.0-1`: `cog` (the reference browser), `gstreamer1.0-wpe` (the
  `wpesrc` GStreamer element) and the package's own `-dev` and `wpewebkit-webdriver`. They keep working by the
  symbol check above (not run one by one; `cog` was the first check of WPE on VM 127).
- **No `libinput`, no `libudev`.** With WPEPlatform's DRM backend on (`+1`), `libWPEWebKit-2.0.so.1` linked both;
  `+2` doesn't (the test checks `NEEDED`). New in `NEEDED` next to 2.48's: WPEPlatform's Wayland backend
  (`libwayland-client`, `libwayland-egl`, `libwayland-server`, `libxkbcommon`), `libatk-bridge`, `libenchant`,
  `libhyphen` and `libgpg-error`; gone: `libwoff2dec` (FreeType decodes WOFF2), Flite and `libgsttranscoder`.
  `libgbm` and `libdrm` were already there in 2.48. Only lord-web loads WebKit; `lord-server` links none of them.

## librice (since `+4`)

WebRTC's ICE (finding a network path between the two sides of a call) needs sockets. WebKit's web process has no
network in its sandbox (bubblewrap's `--unshare-net`), so with `+3` ICE ran there and found nothing. WebKit's answer
is librice: with `-DUSE_LIBRICE=ON`, WebCore's ICE agent speaks librice's sans-I/O protocol (`rice-proto`) and every
socket lives in WebKit's **network process** (`rice-io`), which has the network already; the web process keeps none.
Approved by the maintainer on 2026-11-25. Debian has no librice, so LordOS builds it:

- `tools/pkg/build-librice.sh [revision]` makes `librice 0.4.3-1~lordos13+<revision>` (`librice0`: `librice-proto.so.0`
  and `librice-io.so.0`; `librice-dev`: `rice/rice-proto.h`, `rice/rice-io.h`, `rice-proto.pc`, `rice-io.pc`). WebKit
  2.54 asks for 0.4.2 or later; 0.4.3 is upstream's latest (2026-05-11, Rust 1.85, Debian 13's). `ARCH=arm64` as for
  WebKit.
- **Sources, all pinned** in `tools/pkg/librice.sums`: upstream's tag tarball (github.com/ystreet/librice `v0.4.3`);
  the **vendored crates** (`librice_0.4.3.orig-vendor.tar.xz`: the 291 crates upstream's `Cargo.lock` names, fetched by
  `cargo vendor --locked` in a chroot, so cargo checks each against the lock file's checksum, then packed
  reproducibly: made twice on 2026-11-25, the same SHA-256 both times); and cbindgen 0.29.2 with its 72 crates
  (`librice_0.4.3.orig-cbindgen.tar.xz`; its `.crate` against crates.io's checksum). Only the vendoring step uses the
  network; the package build runs cargo `--frozen --offline`. Prebuilt binaries in the vendored crates (other systems'
  import libraries, test fixtures: 398 and 10 files) are removed, so the source package has none.
- **cbindgen 0.29**: Debian 13's `cargo-c` carries cbindgen 0.27, which doesn't read Rust 2024's `#[unsafe(no_mangle)]`
  and writes empty headers; the package builds cbindgen 0.29.2 from the third tarball and writes both headers with it
  (`debian/headers.sh`, keeping the version defines cargo-c adds). It is never installed.
- `rice-proto` is built `--no-default-features --features capi,openssl`: TURN over TLS through the system's OpenSSL,
  not a second TLS stack (rustls, aws-lc). `librice-proto.so.0` links `libssl.so.3` and `libcrypto.so.3`, which
  `libWPEWebKit-2.0.so.1` already links since `+3`.
- **Licences:** MIT or Apache-2.0 (librice); the vendored crates' own (each crate's `Cargo.toml` → a `debian/copyright`
  stanza, generated; texts from SPDX's list in `tools/pkg/librice/licenses/` or Debian's common licences). The source
  package (`.dsc`, the three orig tarballs, `.debian.tar.xz`) is published next to the binaries exactly as WebKit's
  (below).
- **Test** (`librice-chroot.sh test`, a clean trixie chroot): the packages install, `pkg-config` gives 0.4.3 for both,
  and `tools/pkg/librice-test.c` builds against both and runs: `rice_version` 0.4.3 as the header says, an agent with a
  stream, a UDP socket bound on the loopback by `rice-io`. Result 2026-11-25 (amd64): passes; lintian 0 errors.
- WPE `+4` Build-Depends on `librice-dev (>= 0.4.3)` and, through its shared libraries, depends on `librice0`;
  `build-wpewebkit.sh` takes them with `EXTRA_DEBS=<librice's out/>` (installed before the build dependencies and in the
  test chroot). Publish librice (binaries and source) before or with `+4`.

## Licence and the source next to the binaries

WebKit is mostly LGPL-2+/LGPL-2.1+ (WebCore, JavaScriptCore) and BSD-2-clause/BSD-3-clause (most of WebKit, WTF,
ANGLE, Skia), with smaller parts under MPL-1.1/MPL-2.0, Apache-2.0 (pdf.js, OpenCL headers in ANGLE), ISC, Expat, BSL-1.0,
AFL-2.0 or LGPL-2+ (xdgmime) and BSD-2-Clause-Patent (libsysprof-capture). GPL-2+/GPL-3+ files appear only as
generated Bison parsers (with the Bison exception) and build-time tools; Debian's `debian/copyright` lists every file
group and is installed as `/usr/share/doc/<package>/copyright` in each binary package (the test checks it).

LordOS distributes these binaries, so it offers the exact corresponding source the same way: the source package that
built them, published with them.

- **In the APT repository** (`tools/release/apt-repo.sh`, run only by the maintainer, who signs; [RELEASING.md](RELEASING.md),
  "WPE WebKit (LordOS's backport)"). `build` takes the `.dsc` next to the `.deb`s. Every file the `.dsc` names in
  `Checksums-Sha256` must be next to it with that size and SHA-256. The `.dsc` and those files go, append-only like the
  `.deb`s, to `pool/main/source/<prefix>/<source>/` (`pool/main/source/w/wpewebkit/`). `apt-ftparchive sources` indexes
  them in `dists/stable/main/source/Sources` (`.xz`, `.gz`) before `apt-ftparchive release`, so `Release`, and so
  `InRelease`, lists and signs their hashes. A `.deb` whose version ends in `~lordos<n>+<m>` is refused while its
  source's `.dsc` isn't in the pool, so such a binary is never published without its exact source. `publish` uploads
  the `.dsc` as text and the tarballs as binary, and writes `lordos-src.sources` next to `lordos.sources`: a `deb-src`
  stanza for anyone who wants `apt-get source wpewebkit`. LordOS systems don't get it (the `lordos` package keeps
  installing `lordos.sources`, `deb` only, unchanged; the Store's engine ignores `deb-src`).
  Tested on 2026-11-24 against a scratch tree only, signed with a throwaway key (`LORDOS_APT_OFFLINE=1`,
  `LORDOS_APT_TEST_KEYRING`; `publish` and `verify` refuse that key): the `.debs` alone are refused; a `.dsc` with a
  tampered `.debian.tar.xz` (another size, or the same size and another SHA-256) is refused; the same version again
  with a different `.dsc` is refused (append-only); with the `.dsc` the tree builds, `Release` lists
  `main/source/Sources`, and Debian's `apt` (with a `deb deb-src` stanza on the scratch tree, `Signed-By` the test key)
  updates, offers `2.54.3-1~lordos13+…` as the candidate, and `apt-get source --download-only wpewebkit` fetches
  the `.dsc`, `.orig.tar.xz`, `.asc` and `.debian.tar.xz` with the very SHA-256s of the build.
- **In each image's list.** `tools/release/image-sources.sh` lists a package rebuilt by LordOS (a version ending in
  `~lordos<n>+<m>`) with origin `lordos` and the URL of its `.dsc` in `https://lordos.org/apt/pool/main/source/…`,
  not snapshot.debian.org (Debian never published that version). The images don't include `lordos-web` or WebKit
  today, so the line appears only in an image (or a system's root folder) that has the backport installed.
  [SOURCE-OFFER.md](SOURCE-OFFER.md) names this origin.

## Rebuilding for every WebKit security release

WebKit fixes security bugs in point releases (2.54.x) announced as WebKit Security Advisories,
[WSA-YYYY-NNNN](https://wpewebkit.org/security/), only for the current stable series. Debian uploads each one to
unstable, usually within days. Debian 13 itself still has `wpewebkit` 2.48.3-1, a series WebKit no longer updates
(checked 2026-11-23), which is a second reason for the backport. Once LordOS ships it, **LordOS owns its security
updates**:

1. Watch [wpewebkit.org/security](https://wpewebkit.org/security/) (and its RSS), Debian's
   [wpewebkit tracker](https://tracker.debian.org/pkg/wpewebkit) and the
   [security tracker](https://security-tracker.debian.org/tracker/source-package/wpewebkit).
2. When Debian unstable has the fixed version (`2.54.4-1`, say): `tools/pkg/build-wpewebkit.sh 2.54.4-1` (an amd64
   build takes 26 to 41 minutes of `dpkg-buildpackage` on a 20-core machine with `TMPFS=64G` (seven builds, the slowest with the machine busy), about 50 minutes for the whole script with its tests; arm64 below). A WSA fixed upstream before Debian uploads it: wait for
   Debian's upload (usually a day or two) rather than packaging the tarball alone, so the packaging stays Debian's.
   A rebuild of the same Debian version (a trixie library changed) is `tools/pkg/build-wpewebkit.sh 2.54.3-1 2`.
3. Read `out/lintian-summary.txt` and `out/test/report.txt` (the build stops if the test fails), then the maintainer
   publishes `.deb`s and source together with `tools/release/apt-repo.sh` (staging, check on the VM, then live).
4. A new minor series (2.56) may change the SONAME, the defaults or Debian's packaging: read Debian's changelog and
   `debian/rules` diff, check the ABI section of the test report, and re-test lord-web on the VM before publishing.

Security cadence: WebKit has published a WSA roughly every one to two months, each for the current stable series.
LordOS follows the newest stable series (2.54 today), as Debian unstable does, and moves to the next one (2.56) when
Debian unstable does.

## arm64

Built on the amd64 build machine under qemu-user (maintainer's decision, 2026-11-24; `JOBS` defaults to 8 there, the first run used 16 and was reniced to 19 since it loaded the desktop): `ARCH=arm64 JOBS=16 TMPFS=64G
tools/pkg/build-wpewebkit.sh 2.54.3-1 2`, a full build (source and every package, not `-B`, so `-dev` has the
introspection files as on amd64), in arm64 chroots made by mmdebstrap, every arm64 program run by QEMU through the
binfmt handler (`F` flag, so it works inside the chroots). Result of `+2` (2026-11-24, `/mnt/HD08TB/build/wpewebkit/2.54.3-1~lordos13+2/arm64/out/`): `dpkg-buildpackage` took
42149 s (11.7 h, 16 jobs, most of it at nice 19 next to a busy desktop). The same four packages as on amd64. The
source package is byte for byte amd64's (same `.dsc` and `.debian.tar.xz`, so it is published once). lintian shows
the same 0 errors and 5 warnings. Under qemu the test checks everything that doesn't run WebKit, and it all passes:
`pkg-config`; no `wpe-platform-drm-2.0`; no `libinput`/`libudev` in `NEEDED`; the same SONAME; none of 2.48's 930
symbols removed (1345, 415 added); the copyright files. The headless program and lord-web on both backends are built
with no warnings. Running WebKit isn't possible under qemu-user: bubblewrap can't create its user namespaces there
("Creating new namespace failed"), and the sandbox is never turned off. So under emulation (`ARCH` not the machine's)
the script builds those programs and doesn't run them. That part (the headless render, `web-backend-test` and the
probe test on both backends) needs a real arm64 machine: the Pi 5, with the packages from `out/`, before arm64 is
published. A test stage alone on the Pi: copy the `out/` folder there and run the script with the same version (it
finds the finished build and only tests).

The options considered (2026-11-23):

| Option | How | Time (estimate) | Notes |
|---|---|---|---|
| **Native on the Raspberry Pi 5** (recommended) | Run the script on the maintainer's Pi 5 (Debian 13 arm64, `apt-get install mmdebstrap`) with the build folder on an NVMe or USB SSD, not the SD card: `LORDOS_WPE_WORK=/mnt/ssd/wpewebkit JOBS=3 tools/pkg/build-wpewebkit.sh 2.54.3-1` | 8 to 12 hours | The amd64 build is about 11 core-hours (33 min on 20 cores); a Cortex-A76 core is 2.5 to 3 times slower. WebKit's unified C++ files need up to 2 to 3 GB per compiler, so `JOBS=3` with 8 GB of RAM plus 8 GB of swap on the SSD (`JOBS=4` with 16 GB). About 30 GB of disk. Same chroot, same checks, same test, native. |
| **Emulated on the amd64 build machine** (chosen, 2026-11-24) | `ARCH=arm64 tools/pkg/build-wpewebkit.sh …`: `qemu-user` and its binfmt handlers (with the F flag) are installed there; mmdebstrap makes arm64 chroots (`--architectures=arm64`) and every arm64 program runs under QEMU | 6 to 10 hours on 20 cores (estimate) | Nothing to set up on the Pi, but emulation is 10 to 20 times slower per core. |
| **Cross-compiled** (`dpkg-buildpackage -a arm64`) | Debian doesn't cross-build this package: GObject introspection and the documentation run target binaries, and several build dependencies aren't multi-arch co-installable | under 1 hour if it worked | Not recommended: it would be a different build from Debian's, and needs its own packaging work. |

Whatever the machine, the arm64 build must be a full build (`dpkg-buildpackage` without `-B`), so `libwpewebkit-2.0-dev`
has the GObject introspection files as on amd64 (Debian's rules turn introspection off when the `-doc` package isn't
built); the `libwpewebkit-doc` it makes is the same `all` package and isn't published twice.

## Publishing

Not done by the script, and not done yet: signing and uploading are the maintainer's, on the maintainer's machine
([RELEASING.md](RELEASING.md), "WPE WebKit (LordOS's backport)"; [UPDATES.md](UPDATES.md)). A `lordos-web` built on
lord-web's WPEPlatform backend depends on `libwpewebkit-2.0-1 (>= 2.54)`, which only the LordOS repository has on
trixie, so the backport is published before (or with) the first such `lordos-web`; one built on the legacy backend
depends on `(>= 2.48)` and works with either (`tools/iso/mkdeb-web.sh`).
