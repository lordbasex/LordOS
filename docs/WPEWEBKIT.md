# WPE WebKit for Debian 13: LordOS's backport

LordOS builds its own WPE WebKit for Debian 13 (trixie) and publishes it in the LordOS APT repository, next to its
exact source. Maintainer's decision of 2026-11-23, for [#142](https://github.com/lordbasex/LordOS/issues/142)
(ROADMAP, "Before 1.0": lord-web on the new WPE API). In development: the packages are built and tested, not yet
published; the latest release, 0.8.9.5-beta, still uses Debian's 2.48.

## Why

- `lord-web` ([WEBAPPS.md](WEBAPPS.md)) is the only LordOS binary that links WPE WebKit. Today it uses WebKit's legacy
  API (`libwpe` and `wpebackend-fdo`), which WebKit has deprecated in favour of **WPEPlatform** (`wpe-platform-2.0`).
- Debian 13 ships `wpewebkit` 2.48 built **without** WPEPlatform (no `wpe-platform-2.0.pc`), so lord-web cannot move
  to the new API with trixie's packages. Debian unstable/forky has 2.54 with WPEPlatform.
- The backport is Debian's own source package of the newer version, rebuilt on trixie with as few changes as possible,
  so it keeps Debian's package split, names and SONAME and replaces trixie's 2.48 as an ordinary upgrade. It turns
  both APIs on: WPEPlatform for the port, and the legacy API so today's lord-web keeps working until the port lands.

## What gets built

`tools/pkg/build-wpewebkit.sh <debian-version> [lordos-revision]`, for example `tools/pkg/build-wpewebkit.sh 2.54.3-1`,
makes version `2.54.3-1~lordos13+1` (`~` sorts it below Debian's own `2.54.3-1`, so a later Debian package of the same
version wins; `lordos13` is Debian 13). Its steps, each in a clean chroot made by `mmdebstrap` (a Debian build tool,
installed on the build machine with `apt-get install mmdebstrap`; no container runtime):

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
   them; `pkg-config` finds `wpe-webkit-2.0`, `wpe-platform-2.0`, `wpe-platform-drm-2.0`, `wpe-platform-headless-2.0`,
   `wpe-platform-wayland-2.0`, and the legacy `wpe-1.0` and `wpebackend-fdo-1.0`; the SONAME and the exported symbols
   are compared with trixie's own `libwpewebkit-2.0-1` 2.48; each package has its `/usr/share/doc/<package>/copyright`;
   and `tools/pkg/wpewebkit-headless-test.c`, compiled there, opens a `WebKitWebView` on WPEPlatform's headless
   display, loads a page of a known colour and reads the frame WebKit hands to the view (`buffer-rendered`) back to
   pixels, with WebKit's web process in its bubblewrap sandbox as on a desktop. The kernel refuses user namespaces
   inside a chroot, so this stage runs as the root of its own mount namespace (`unshare` plus
   `tools/pkg/wpewebkit-pivot.sh`, which does `pivot_root` into the chroot with a fresh `/proc`, `/sys` and a minimal
   `/dev`: no GPU, so the frame is WebKit's CPU path, a `WPEBufferSHM`). `TEST_ONLY=1` repeats this stage alone.

Checked separately on 2026-11-23: today's `lord-web` (legacy API) builds against the backport with `-Wall -Wextra` and
no warnings (`make -C server build/lord-web` in the test chroot, with trixie's `libwpebackend-fdo-1.0-dev`), and
`tools/release/image-sources.sh` run on the test chroot lists `wpewebkit 2.54.3-1~lordos13+1` with origin `lordos` (176
other source packages `debian`). In the same chroot, with WebKit's sandbox on, lord-web's own probe smoke test
(`server/tests/web_probe_test.py`: `--probe` and `--probe-icon` against a page it serves, legacy API, no window)
passes, peak 303 MB PSS for lord-web and its WebKit processes. Not checked yet: `lord-web`'s windows on the VM.

Output, in `$LORDOS_WPE_WORK/<version>/out/` (default `/var/tmp/lordos-wpewebkit`; on the maintainer's build machine
`/mnt/HD08TB/build/wpewebkit`): the `.deb`s, the **source package** (`wpewebkit_<version>.dsc`,
`wpewebkit_<upstream>.orig.tar.xz`, `wpewebkit_<upstream>.orig.tar.xz.asc`, `wpewebkit_<version>.debian.tar.xz`),
`.changes`, `.buildinfo`, `SHA256SUMS`, `build-time.txt`, `lintian.txt` and `lintian-summary.txt`, and `test/` (the
test report and the rendered frame). Nothing is signed or published by the script.

### LordOS's changes to Debian's packaging

All in `backport()` of `tools/pkg/wpewebkit-chroot.sh`; a change that stops applying stops the build and says so.

| Change | Why |
|---|---|
| `debian/rules`: `-DENABLE_WPE_PLATFORM=ON`, `-DENABLE_WPE_PLATFORM_DRM=ON`, `-DENABLE_WPE_PLATFORM_HEADLESS=ON`, `-DENABLE_WPE_PLATFORM_WAYLAND=ON`, `-DENABLE_WPE_LEGACY_API=ON` | Upstream's defaults in 2.54 already, but written out: a release that changes a default must fail here, not ship without WPEPlatform or without the legacy API lord-web still uses |
| `debian/control`: `Maintainer: LordOS`, `Uploaders` removed | Bugs in this build come to LordOS, not to Debian. Debian's WebKit team stays credited in the changelog and `debian/copyright` (not as `XSBC-Original-Maintainer`: Debian's lintian doesn't know that field) |
| `debian/changelog`: entry `<version>~lordos13+<n>` for `trixie`, dated one second after the Debian entry it follows | The backport's version, below Debian's own. A fixed date (later than Debian's, as lintian requires) is the build's `SOURCE_DATE_EPOCH`, so building the same version again gives the same `.debian.tar.xz` |

Nothing else changes: every build dependency of 2.54.3-1 is in trixie (clang 19 instead of sid's newer default
`clang`, libsoup 3, GLib 2.84, GStreamer 1.26, libjxl, libavif, libwebp, HarfBuzz, libgbm, libinput, libwayland,
wayland-protocols), and no feature had to be turned off: with the same build dependencies and cmake options as Debian unstable, the features are unstable's
(no Manette gamepads, no WebRTC, no encrypted media, as there). WOFF2 web fonts come from FreeType now (Debian dropped
`libwoff-dev` in 2.54.0-1: trixie's FreeType 2.13, built with Brotli, decodes WOFF2 itself). The `clang-22 [armhf]` build dependency can't be met on
trixie, but LordOS doesn't build armhf.

### The packages

Debian's split, unchanged: `libwpewebkit-2.0-1` (the library, WebKit's processes, MiniBrowser), `libwpewebkit-2.0-dev`
(headers, `.pc` files, GObject introspection `.gir`/`.typelib`), `wpewebkit-webdriver` and `libwpewebkit-doc`
(architecture `all`). There is **no new binary package for WPEPlatform**: upstream builds it into
`libWPEWebKit-2.0.so.1` itself, and its headers and `wpe-platform-2.0.pc`, `wpe-platform-drm-2.0.pc`,
`wpe-platform-headless-2.0.pc`, `wpe-platform-wayland-2.0.pc` are in `libwpewebkit-2.0-dev`. The build also makes
`-dbgsym` packages (debug symbols, about 140 MB for the library); they stay in `out/` and don't need to be published.

### ABI and replacing trixie's 2.48

Measured by the test against trixie's `libwpewebkit-2.0-1` 2.48.3-1 (2026-11-23):

- **Same SONAME**, `libWPEWebKit-2.0.so.1` (trixie's file is `.so.1.5.8`, the backport's `.so.1.11.6`): the same
  package name, `libwpewebkit-2.0-1`, upgrades in place.
- **No exported symbol removed**: 2.48 exports 930 dynamic symbols, the backport 1354, every one of the 930 among them
  (424 added, WPEPlatform's `wpe_*` API among them). Programs built against 2.48 (today's `lord-web`, `cog`) keep
  linking; Debian's `debian/libwpewebkit-2.0-1.symbols` for 2.54.3 is checked by `dpkg-gensymbols` during the build too.
- The legacy API stays: `wpe-1.0` (libwpe 1.16.2) and `wpebackend-fdo-1.0` (1.16.0) are trixie's own packages and
  still resolve next to the new `.pc` files.
- What else in trixie depends on `libwpewebkit-2.0-1`: `cog` (the reference browser), `gstreamer1.0-wpe` (the
  `wpesrc` GStreamer element) and the package's own `-dev` and `wpewebkit-webdriver`. They keep working by the
  symbol check above (not run one by one; `cog` was the first check of WPE on VM 127).
- New in the library: WPEPlatform's DRM backend links `libinput` and `libudev`, and its Wayland backend
  `libwayland-client`, inside `libWPEWebKit-2.0.so.1`, so `lord-web` (the only LordOS binary that loads WebKit) now
  maps them too. They are loaded only by WebKit's library; `lord-server` never links any of them (ARCHITECTURE.md's
  policy is about LordOS's own binaries). Building without the DRM backend (`-DENABLE_WPE_PLATFORM_DRM=OFF`) would drop
  `libinput`; LordOS keeps it on as decided, since lord-web uses the Wayland backend and never opens the DRM one.

## Licence and the source next to the binaries

WebKit is mostly LGPL-2+/LGPL-2.1+ (WebCore, JavaScriptCore) and BSD-2-clause/BSD-3-clause (most of WebKit, WTF,
ANGLE, Skia), with smaller parts under MPL-1.1/MPL-2.0, Apache-2.0 (pdf.js, OpenCL headers in ANGLE), ISC, Expat, BSL-1.0,
AFL-2.0 or LGPL-2+ (xdgmime) and BSD-2-Clause-Patent (libsysprof-capture). GPL-2+/GPL-3+ files appear only as
generated Bison parsers (with the Bison exception) and build-time tools; Debian's `debian/copyright` lists every file
group and is installed as `/usr/share/doc/<package>/copyright` in each binary package (the test checks it).

LordOS distributes these binaries, so it offers the exact corresponding source the same way: the source package that
built them, published with them.

- **In the APT repository.** `tools/release/apt-repo.sh` (run only by the maintainer, who signs) indexes `.deb`s today
  and keeps the copyleft sources of the Raspberry Pi packages unindexed in `pool/rpi/source/`. For this backport the
  source package goes in `pool/main/source/<prefix>/<source>/` (`pool/main/source/w/wpewebkit/`) and is indexed as
  `deb-src` in `dists/stable/main/source/Sources`, so `apt-get source wpewebkit` works on a LordOS system with a
  `deb-src` line. The change to `apt-repo.sh` that does it (not made yet, see "Publishing"): accept `.dsc` arguments
  next to the `.deb`s; copy each `.dsc` and every file its `Checksums-Sha256` names, after checking those sums, into
  that folder (append-only, like the `.deb`s); write `main/source/Sources` with
  `apt-ftparchive sources pool/main/source` (plus `.xz`/`.gz`) before `apt-ftparchive release`, which then lists and
  signs its hashes; add `deb-src` (`Types: deb deb-src`) to the published `lordos.sources` only if LordOS wants systems
  to see it by default (it isn't needed for the offer); and make `build` refuse a `.deb` whose source version has no
  `.dsc` in the pool, so a binary is never published without its source.
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
   build takes about 30 minutes of `dpkg-buildpackage` on a 20-core machine with `TMPFS=64G`, about 45 minutes for the whole script; arm64 below). A WSA fixed upstream before Debian uploads it: wait for
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

Not built yet. `build-wpewebkit.sh` builds for the machine it runs on, so arm64 is one of:

| Option | How | Time (estimate) | Notes |
|---|---|---|---|
| **Native on the Raspberry Pi 5** (recommended) | Run the script on the maintainer's Pi 5 (Debian 13 arm64, `apt-get install mmdebstrap`) with the build folder on an NVMe or USB SSD, not the SD card: `LORDOS_WPE_WORK=/mnt/ssd/wpewebkit JOBS=3 tools/pkg/build-wpewebkit.sh 2.54.3-1` | 8 to 12 hours | The amd64 build is about 11 core-hours (33 min on 20 cores); a Cortex-A76 core is 2.5 to 3 times slower. WebKit's unified C++ files need up to 2 to 3 GB per compiler, so `JOBS=3` with 8 GB of RAM plus 8 GB of swap on the SSD (`JOBS=4` with 16 GB). About 30 GB of disk. Same chroot, same checks, same test, native. |
| **Emulated on the amd64 build machine** | `qemu-user` and its binfmt handlers are already installed there; mmdebstrap makes an arm64 chroot with `--architectures=arm64` and every compiler runs under QEMU (a small change: an `ARCH` variable passed to both mmdebstrap calls) | 6 to 10 hours on 20 cores | Nothing to set up on the Pi, but emulation is 10 to 20 times slower per core, and bubblewrap's user namespaces under QEMU are untested, so the sandboxed render test may need the Pi anyway. |
| **Cross-compiled** (`dpkg-buildpackage -a arm64`) | Debian doesn't cross-build this package: GObject introspection and the documentation run target binaries, and several build dependencies aren't multi-arch co-installable | under 1 hour if it worked | Not recommended: it would be a different build from Debian's, and needs its own packaging work. |

Whatever the machine, the arm64 build must be a full build (`dpkg-buildpackage` without `-B`), so `libwpewebkit-2.0-dev`
has the GObject introspection files as on amd64 (Debian's rules turn introspection off when the `-doc` package isn't
built); the `libwpewebkit-doc` it makes is the same `all` package and isn't published twice.

## Publishing

Not done by the script, and not done yet: signing and uploading are the maintainer's, on the maintainer's machine
([RELEASING.md](RELEASING.md), [UPDATES.md](UPDATES.md)). `lordos-web` (`tools/iso/mkdeb-web.sh`) keeps depending on
`libwpewebkit-2.0-1 (>= 2.48)`, so it works with trixie's package or the backport; once lord-web uses WPEPlatform it
will depend on `libwpewebkit-2.0-1 (>= 2.54)`, which only the LordOS repository has on trixie.
