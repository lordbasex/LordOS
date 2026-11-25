# Releasing LordOS

How a LordOS version goes from `main` to a [GitHub release](https://github.com/lordbasex/LordOS/releases).
The releases page is the project's public changelog: every release is documented in full, from
[CHANGELOG.md](../CHANGELOG.md).

## Versions

- [Semantic Versioning](https://semver.org/spec/v2.0.0.html). Until 1.0, every release is a beta.
- The CHANGELOG version and the git tag carry the pre-release suffix: `0.8.0-beta`, tag
  `v0.8.0-beta`.
- `package.json` carries the version without the suffix (`"version": "0.8.0"`) and the channel
  separately (`"lordos": {"channel": "beta"}`; empty for a stable release). The file names use
  that version: `lordos_0.8.0_amd64.deb`, `lordos-live-0.8.0-amd64.iso`,
  `lordos-0.8.0-rpi5-arm64.img.xz`, since 0.8.7 `lordos-live-0.8.7-arm64.iso` and, since 0.8.8,
  `lordos-0.8.8-amd64.ova`.
- **An update of a published version** that only fixes it or completes it (no features planned for
  the next version) adds a fourth number: `0.8.4.1-beta` after `0.8.4-beta`, package `0.8.4.1`. dpkg
  sorts it after `0.8.4` and before `0.8.5`, so Store → Updates and apt offer it like any version.
  (Semantic Versioning has no fourth number; LordOS uses it only for these.)
- The release title is **`LordOS <version>`**, for example `LordOS 0.8.0-beta`.
- Every beta (any version with a suffix) is published as a **pre-release**. Only a version without
  a suffix is a normal release.

## The changelog

[CHANGELOG.md](../CHANGELOG.md) follows [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/):

- Changes go under `## [Unreleased]` as they land, in `### Added`, `### Changed`, `### Deprecated`,
  `### Removed`, `### Fixed` and `### Security` (LordOS also uses `### Known issues and
  limitations`, `### Upgrading` and `### Verifying`).
- Bullets are written for users first (what you can do now, what was wrong), then technical detail
  where useful, with links to the docs (`docs/…`).
- Only what the code and the docs show. Measured numbers come from the test docs
  ([ISO.md](ISO.md), [RASPBERRY.md](RASPBERRY.md)…).
- The wording rules apply: a free GNU/Linux distribution based on Debian with its own desktop
  environment; generic names, no commercial brand names; English only.

## Steps

1. **Changelog.** Rename `## [Unreleased]` to `## [<version>] - <YYYY-MM-DD>` (the release date),
   add a new empty `## [Unreleased]` above it, and update the link references at the bottom:

   ```
   [Unreleased]: https://github.com/lordbasex/LordOS/compare/v<version>...HEAD
   [<version>]: https://github.com/lordbasex/LordOS/compare/v<previous>...v<version>
   ```

   Also bump `package.json` (`version`, `lordos.channel`) and commit both.

   **Machines' LordOS card** (since 0.8.9.5): when this release ships new live ISOs, point the `lordos`
   entry of `assets/machines/systems.json` at it in the same commit: both `dir` folders
   (`https://github.com/lordbasex/LordOS/releases/download/v<version>/`), `release` (the version without its suffix),
   `approxMb` (each ISO's size in MiB, from the build), and `checked`, and both `list.dir` folders
   (`https://lordos.org/releases/v<version>/`, the copy of the signed list Machines reads first; step 9 uploads it).
   Its `SHA256SUMS` and `SHA256SUMS.sig` are the ones step 5 writes, so nothing else changes. A release without ISOs leaves it alone. `vm_catalog_test.mjs` fails
   once `web/content/downloads.json` names an ISO newer than the card's tag ([VIRTUALIZATION.md](VIRTUALIZATION.md),
   3.9, "The LordOS card").

   **Web apps' Browser identity** (0.8.8.9): set `WEBPAGE_CHROME_MAJOR`, `WEBPAGE_EDGE_MAJOR` and `WEBPAGE_FIREFOX_MAJOR`
   in `server/src/web/webpage.h` to the current stable desktop majors (Chrome's from the Chrome Releases blog, Edge's
   from its Stable channel release notes, Firefox's from its release notes) and update the strings quoted in `docs/WEBAPPS.md` ("Browser identity") and in the
   changelog entry if they changed. A stale major is what makes sites treat a browser as outdated. `build/webapp-test`
   checks only that the numbers are plausible, never the date, so this step is the only reminder.
2. **Tag.** `git tag -a v<version> -m "LordOS <version>"` on that commit, and push the tag.
3. **Sign the system code, then build** from the tag.
   - On the maintainer's machine (where the release key lives): `npm run build`, then
     `tools/release/sign-code.sh`. It regenerates `code-manifest.json` (the SHA-256 of every theme
     bundle, the app host and the system apps) and writes `code-manifest.sig` with the release key,
     checked against `tools/release/lordos-release.pub.pem` ([SIGNING.md](SIGNING.md), "Verified system
     code"). The bundles are the same for every architecture: one signature serves all the images.
     (0.8.7) Also `tools/release/sign-hwsnapshot.sh`: the tested-hardware snapshot
     (`assets/hwreport/snapshot.json.sig`, same key and format), which ships in its own package, `lordos-hwdata`
     (below); `REQUIRE_SIGNED_CODE=1 tools/iso/mkdeb-hwdata.sh` refuses an unsigned one or one whose signature
     doesn't verify with `lordos-release.pub.pem`.
   - Send the tree, with both files, to the build VMs (never the key). Build lord-server as a release:
     `make -C server RELEASE=1` (it only runs code signed with the release key, with no development
     mode).
   - Build the ISO on the build VM (`sudo REQUIRE_SIGNED_CODE=1 tools/iso/build.sh`, [ISO.md](ISO.md)),
     then, in the same folder, the OVA from that ISO's system (0.8.8; `sudo tools/iso/build-ova.sh`, needs `qemu-utils` and
     `libarchive13t64`; it writes `lordos-<version>-amd64.ova` and its `.sha256` next to the ISO. It has no signature of its
     own: `SHA256SUMS` covers it. Check it with `VBoxManage import`, and with `ovftool`, which reads it like VMware does),
     the arm64 ISO (0.8.7; `sudo RELEASE=1 REQUIRE_SIGNED_CODE=1 tools/iso/build-arm64.sh`, in a copy of the same
     signed tree, because it compiles `server/build` for aarch64),
     (the first release that ships it also adds arm64 to the ISO card in `web/src/home.html` and the file name in
     `web/lib/imager-guide.mjs`, and to README's downloads; they stay amd64 until the file exists)
     the Raspberry Pi 5 image (`sudo RELEASE=1 REQUIRE_SIGNED_CODE=1 tools/rpi/build-img.sh`,
     [RASPBERRY.md](RASPBERRY.md)) and the `.deb` for amd64 and arm64
     (`REQUIRE_SIGNED_CODE=1 tools/iso/mkdeb.sh`), plus (0.8.7) `lordos-disk-formats_<version>_<arch>.deb` on each architecture (`tools/iso/mkdeb-diskformats.sh <folder>`, the Store's Disk Formats item, published with `lordos` and never inside the images), plus `lordos-studio-camera_<version>_<arch>.deb` on each architecture (`tools/iso/mkdeb-studiocamera.sh <folder> <arch>`, the Store's LordOS Studio Camera card, [CAMERA-STUDIO-PLAN.md](CAMERA-STUDIO-PLAN.md) 11.2; never inside the images),
     and the one `lordos-hwdata_<date>_all.deb` for both
     (`REQUIRE_SIGNED_CODE=1 tools/iso/mkdeb-hwdata.sh <folder>`; the ISO and image builds make it too and install
     it with `lordos`, which depends on it), plus (0.8.8.5) `lordos-rclone_<version>_<arch>.deb` for each architecture
     (`tools/iso/mkdeb-rclone.sh <folder>`: rclone's signed upstream binary, unmodified; `lordos` depends on it) and
     (0.8.8.8) `lordos-web_<version>_<arch>.deb`, the LordOS Web Runtime, on each architecture after `make -C server`
     with `libwpewebkit-2.0-dev` and `libwpebackend-fdo-1.0-dev` installed (`tools/iso/mkdeb-web.sh <folder>`; it
     depends on that exact `lordos` version and is optional, never inside the images). The package is reproducible: built again from the same snapshot and
     signature on another VM it has the same bytes. `mkdeb.sh` puts `code-manifest.json` and
     `code-manifest.sig` in `/usr/lib/lordos/` and runs `lord-server --check-code` on the staged tree:
     the package is not built if a bundle changed after signing or the signature is missing. A bundle
     rebuilt after signing means signing again. An update with a fourth number may ship without a
     new ISO (0.8.4.1 did: the 0.8.4 ISO stayed the current one, see [ISO.md](ISO.md)).
   - Copy them, and `tools/install/install.sh` as `install.sh`, into one release folder (with
     `lordos-hwdata_<date>_all.deb` and both `lordos-rclone` packages: `install.sh` installs them from the release
     together with `lordos`, since a Debian system has no LordOS repository yet; `sign-release.sh` refuses a folder
     without them, unless `ALLOW_NO_HWDATA=1` or `ALLOW_NO_RCLONE=1` says that `lordos` doesn't depend on them).
4. **SBOM and source lists.** On each build VM, `tools/release/make-sbom.sh lordos_<v>_<arch>.deb`, and put the
   `.spdx.json` files in the release folder ([COMPLIANCE.md](COMPLIANCE.md)). Every image goes with its source
   list: `build.sh`, `build-ova.sh` and `build-img.sh` write `<image>.sources.tsv` next to it (below, "Source
   code"); put each one in the release folder with its image, so `SHA256SUMS` covers it too. A release with no new
   images has none.
5. **Sign.** On the maintainer's machine, `tools/release/sign-release.sh <release-dir>`: it writes
   `SHA256SUMS` and `SHA256SUMS.sig` and checks them against the public key
   ([SIGNING.md](SIGNING.md), "Release files"). Then copy `tools/release/lordos-release.pub.pem`
   into the folder too (it is not covered by `SHA256SUMS`: it is checked by its fingerprint).
6. **Release notes.** `tools/release/release-notes.sh <version> > notes.md` prints the
   changelog section for that version (relative links made absolute for the tag, hard-wrapped
   lines joined, its "Verifying" part replaced by the footer) plus a fixed footer: the downloads
   table, the verification steps and links to lordos.org, the paper, the docs and the security
   page. Read it once before publishing. The downloads table takes one Imager version for every
   platform, `lordos.imager` in `package.json`; when the platforms' Imager builds come from different
   releases (0.8.8.8: the Mac build is 0.8.6's, Windows and Linux 0.8.8.6's), fix those links by hand. It
   lists no row for `lordos-web` or `lordos-disk-formats`.
7. **The APT repository** (what installed systems update from; [UPDATES.md](UPDATES.md)). On the
   maintainer's Mac, where the archive key lives (never on a VM):

   ```sh
   # staging first: the same .debs, the notes from this version's CHANGELOG section
   tools/release/apt-repo.sh build staging <release-dir>/lordos-hwdata_<date>_all.deb <release-dir>/lordos-disk-formats_<file-version>_amd64.deb <release-dir>/lordos-disk-formats_<file-version>_arm64.deb <release-dir>/lordos-studio-camera_<file-version>_amd64.deb <release-dir>/lordos-studio-camera_<file-version>_arm64.deb <release-dir>/lordos-rclone_<rclone-version>_amd64.deb <release-dir>/lordos-rclone_<rclone-version>_arm64.deb <release-dir>/lordos_<file-version>_amd64.deb <release-dir>/lordos_<file-version>_arm64.deb <release-dir>/lordos-web_<file-version>_amd64.deb <release-dir>/lordos-web_<file-version>_arm64.deb
   tools/release/apt-repo.sh publish staging
   tools/release/apt-repo.sh verify staging        # https and http, signature, every index hash
   ```

   On a clean VM with the previous release installed, point a copy of `lordos.sources` at
   `https://lordos.org/apt-staging` (or a `file:` copy of the tree) and check: `sudo apt update`
   is clean, Store → Updates shows "LordOS <version> is available" with these notes, Update installs
   it, the migrations run (`ls /var/lib/lordos/migrations`), the user's settings are untouched,
   "Restart required" appears and the system comes back after the restart; a second `apt install
   --reinstall lordos` runs no migration again. On the Raspberry Pi, `apt-get update` against
   staging in a temporary `Dir::State::Lists` (no change to the board) validates the arm64 index.
   Then the same for the live repository:

   ```sh
   tools/release/apt-repo.sh build live <release-dir>/lordos-hwdata_<date>_all.deb <release-dir>/lordos-disk-formats_<file-version>_amd64.deb <release-dir>/lordos-disk-formats_<file-version>_arm64.deb <release-dir>/lordos-studio-camera_<file-version>_amd64.deb <release-dir>/lordos-studio-camera_<file-version>_arm64.deb <release-dir>/lordos-rclone_<rclone-version>_amd64.deb <release-dir>/lordos-rclone_<rclone-version>_arm64.deb <release-dir>/lordos_<file-version>_amd64.deb <release-dir>/lordos_<file-version>_arm64.deb <release-dir>/lordos-web_<file-version>_amd64.deb <release-dir>/lordos-web_<file-version>_arm64.deb
   tools/release/apt-repo.sh publish live
   tools/release/apt-repo.sh verify live
   ```

   A version is never re-uploaded with other contents (the script refuses); a fix is a new version.
   (0.8.8.5) It refuses a `lordos` that depends on `lordos-rclone` while the pool has none of its architecture.
   (0.8.7) The script also refuses a `lordos` that depends on `lordos-hwdata` while the pool has none, and a
   `lordos-hwdata` whose snapshot signature doesn't verify with `lordos-release.pub.pem`. When the snapshot didn't
   change since it was last published, passing the same `lordos-hwdata` again is fine (same bytes).
   `verify live` must pass **before** any release whose package carries `lordos.sources` is
   published: otherwise `apt update` fails on every system that installs it. A change between
   versions that is not just new files (a unit to enable, a renamed setting, a config to convert)
   is a migration in `packaging/migrations/` or `sdk/settings.js` ([UPDATES.md](UPDATES.md),
   "Migrations"), never a manual step in the release notes.
8. **Results.** Collect the evidence on each test machine with `tools/results/collect.sh <machine>` (the
   release test VMs, the Raspberry Pi 5 with a monitor), fill in each README's conditions, commit
   `results/<version>/`, and add `tar czf results-<version>.tar.gz results/<version>` to the release
   files ([RESULTS.md](RESULTS.md)).
9. **Publish:**

   ```sh
   gh release create v<version> --title "LordOS <version>" --prerelease --verify-tag \
       --notes-file notes.md <release-dir>/*
   ```

   Leave out `--prerelease` only for a version without a suffix. The Debian installer
   (`install.sh`) finds betas too: `releases/latest` skips pre-releases, so it falls back to the
   newest release of any kind ([INSTALL-DEBIAN.md](INSTALL-DEBIAN.md)).

   **The signed list on lordos.org** (every release; Machines' LordOS card reads it there first, because GitHub's
   release downloads sometimes answer 403 on their signed redirect: [VIRTUALIZATION.md](VIRTUALIZATION.md), 3.9, "The
   LordOS card"). On the maintainer's Mac:

   ```sh
   tools/release/publish-lists.sh v<version> <release-dir>
   ```

   It checks `SHA256SUMS.sig` against `tools/release/lordos-release.pub.pem` first and uploads nothing if it doesn't
   verify, then copies `SHA256SUMS` as `releases/v<version>/SHA256SUMS.txt` and `SHA256SUMS.sig` as
   `releases/v<version>/SHA256SUMS.sig` into the site's bucket (`text/plain; charset=utf-8`, `no-cache`; the profile,
   bucket and distribution from `tools/web/aws-env.sh`, like `deploy.sh`), invalidates `/releases/*` and compares
   what the site serves with the release's files. By hand it is the same:

   ```sh
   aws s3 cp <release-dir>/SHA256SUMS s3://<site bucket>/releases/v<version>/SHA256SUMS.txt \
       --cache-control no-cache --content-type "text/plain; charset=utf-8"
   aws s3 cp <release-dir>/SHA256SUMS.sig s3://<site bucket>/releases/v<version>/SHA256SUMS.sig \
       --cache-control no-cache --content-type "text/plain; charset=utf-8"
   aws cloudfront create-invalidation --distribution-id <distribution> --paths "/releases/*"
   ```

   The list needs its `.txt`: CloudFront serves an extensionless path as a pretty-URL page (a 404). `deploy.sh` never
   deletes `releases/`, so older tags stay for the installed systems whose card still names them. When the card's
   pinned tag moves (step 1), its `list.dir` moves with it, and this upload must be done before that build ships; if
   it is missing or wrong, the card falls back to the GitHub list (same signature, same key), so it is never a
   security problem, only the 403 workaround lost.
10. **Check:** download from the release page and run the verification commands from the notes;
   run `curl -fsSL https://lordos.org/install.sh | sh` on a clean Debian 13; update the website if
   it names the current version.

## Source code

The images carry GPL and other copyleft binaries (Debian's packages, the Raspberry Pi archive's kernel and
FFmpeg, LordOS's own Raspberry Pi kernel), so each one is published with the list of the exact source packages
it contains and where each source is, under the written offer in [SOURCE-OFFER.md](SOURCE-OFFER.md)
([lordos.org/legal/source-code](https://lordos.org/legal/source-code/)). For every image of a release:

1. **The list comes from the build.** `tools/iso/build.sh` writes `lordos-live-<version>-<arch>.sources.tsv`,
   `tools/iso/build-ova.sh` `lordos-<version>-amd64.sources.tsv` and `tools/rpi/build-img.sh`
   `lordos-<version>-rpi5-arm64.sources.tsv`, all with `tools/release/image-sources.sh` on the image's system before
   the APT lists are removed, so each package's origin (Debian or Raspberry Pi) is exact. Read what it prints:
   a `warning:` (a package in no APT list, a kernel with no known commit) is fixed before publishing, and the
   `note:` names the Raspberry Pi sources LordOS keeps no copy of. A copyleft one there (a newer FFmpeg, the
   archive's own kernel for a board LordOS has no kernel for) gets its `source` line in
   `tools/release/rpi-mirror.list` and is published with `apt-repo.sh` before the image is. Raspberry Pi's signed
   index keeps only the newest version of each source: when the image's version is already gone from it, the line
   is `source-pool` instead, with the SHA-256 of the `.dsc` from Raspberry Pi's pool
   (`curl -fsS https://archive.raspberrypi.com/debian/pool/main/<letter>/<source>/<file>.dsc | sha256sum`), and
   whatever else can be checked independently (an `.orig.tar.xz` that kernel.org or the signed index also lists)
   goes in the list's comment.
2. **The kernel's commit and its source.** `build-img.sh` passes `$KERNEL_DEBS/SOURCE` (written by
   `tools/rpi/build-kernel.sh`) so the Pi kernel's line names the exact commit. A kernel built from a commit
   `image-sources.sh` doesn't know yet (its `KNOWN_KERNELS`) also goes into [RASPBERRY.md](RASPBERRY.md), "The LordOS
   kernel". LordOS keeps each kernel's source itself (`image-sources.sh` warns when it doesn't):
   `build-kernel.sh` leaves `out/source/linux-lordos-rpi-<flavour>_<version>/` (the exact `.config`, the script,
   `SOURCE` with the tree's `tar_sha256`); commit it as `tools/rpi/kernel/linux-lordos-rpi-<flavour>_<version>/`
   and add the `tarball` line, with the SHA-256 of GitHub's tarball of the commit after checking it is that tree:

   ```sh
   curl -fsSL -o tree.tar.gz https://codeload.github.com/raspberrypi/linux/tar.gz/<commit>
   gzip -dc tree.tar.gz | sha256sum      # must be SOURCE's tar_sha256
   sha256sum tree.tar.gz                 # the tarball line's SHA-256
   ```

   `apt-repo.sh build <target> --rpi` publishes it in `pool/rpi/source/l/linux-lordos-rpi-<flavour>/` before the
   image goes out.
3. **Publish it with the image**, in the release folder before `sign-release.sh` (step 4 above), so it is in
   `SHA256SUMS`, and keep it there: the offer runs for three years after the image was last published.
4. **Requests** arrive as GitHub issues from the "Source code request" form (`.github/ISSUE_TEMPLATE/source-request.yml`,
   label `source-request`; create the label once with `gh label create source-request --repo lordbasex/LordOS`).
   Answer each one with the source asked for: a Debian source that snapshot.debian.org no longer serves, or a
   Raspberry Pi one the archive dropped, is uploaded to the release or to `lordos.org/apt/pool/`, and the answer
   links it.

**Images published before the lists** (0.8.8 and earlier): their versions are only in the images. On a Debian
machine with the published files (no root needed; [SOURCE-OFFER.md](SOURCE-OFFER.md) lists the tools):

```sh
tools/release/image-sources.sh lordos-live-0.8.8-amd64.iso
tools/release/image-sources.sh lordos-live-0.8.8-arm64.iso
tools/release/image-sources.sh lordos-0.8.8-amd64.ova
TMPDIR=<a folder with room for the uncompressed image> tools/release/image-sources.sh lordos-0.8.8-rpi5-arm64.img.xz
gh release upload v0.8.8-beta lordos-live-0.8.8-amd64.sources.tsv lordos-live-0.8.8-arm64.sources.tsv \
    lordos-0.8.8-amd64.sources.tsv lordos-0.8.8-rpi5-arm64.sources.tsv --repo lordbasex/LordOS
```

They are added to the release as they are, outside its signed `SHA256SUMS` (which stays as published); the
images they describe are covered by it, and the lists only name public sources.

## The labels of hardware reports

The `hardware-report` issue form and its workflow (`.github/workflows/hardware-report.yml`, on `main`) label each
report from fixed lists (`ALL_LABELS` in `tools/hwreport/lib.mjs`, 42 labels: since the toolkit audit, also
`toolkit:<combination>-fail` for a report whose optional "toolkits" section has a combination that crashed or scored
under 80 %, docs/TOOLKIT-AUDIT.md). Create them once, by hand, before the
workflow reaches `main`: the form can only apply `hardware-report` if the repository already has it. It takes an
account with write access to the repository and `gh auth login` done; `gh label create --force` updates a label
that already exists, so it can be run again. `server/tests/hwreport_test.mjs` fails if this list and the code differ.

<!-- hardware labels -->
```sh
R=lordbasex/LordOS
for l in hardware-report \
    report:valid report:check report:invalid report:missing \
    kind:vm kind:board kind:laptop kind:server kind:desktop \
    gpu:intel gpu:amd gpu:nvidia gpu:virtual gpu:broadcom gpu:arm gpu:vivante gpu:qualcomm gpu:other gpu:none \
    cpu:intel cpu:amd cpu:arm cpu:riscv cpu:other \
    live installed \
    boot:internal boot:usb boot:sd boot:emmc boot:optical boot:network \
    toolkit:gtk3-wayland-fail toolkit:gtk3-x11-fail toolkit:gtk4-wayland-fail toolkit:gtk4-x11-fail \
    toolkit:qt5-wayland-fail toolkit:qt5-xcb-fail toolkit:qt6-wayland-fail toolkit:qt6-xcb-fail toolkit:x11-xwayland-fail; do
  gh label create "$l" --repo "$R" --force
done
```

Check with `gh label list --repo lordbasex/LordOS --limit 100`. Done for 0.8.7: 33 labels created. The 9
`toolkit:*` labels are new (in development): run the loop again before the release that ships the toolkit audit.
with a fake fetch), never on a real issue, because workflows for `issues` events run from the default branch.

## Voice packages

(0.8.8.6; [VOICE.md](VOICE.md), "Models, voices and packages".) The models and voices for reading aloud are their own
packages, `Architecture: all`, published only when they change (a new Kyutai model or voice), not with every release:

```sh
tools/voice/fetch-voices.sh <dir>             # Kyutai's files, SHA-256 checked (needs a Hugging Face read token)
tools/voice/make-previews.sh <dir>            # the recorded samples, with server/build/lord-say and lord-tts
node tools/voice/mkdeb-voice.mjs <dir> <out> <YYYY.MM.DD>
tools/release/apt-repo.sh build staging <out>/*.deb      # then live, as for any package
```

## WPE WebKit (LordOS's backport)

(In development; [WPEWEBKIT.md](WPEWEBKIT.md).) LordOS's own build of Debian's `wpewebkit` for trixie, with
WPEPlatform. Published when a WebKit security release or a Debian upload needs it, not with every release, and
**always with its source package**: the binaries are LGPL-2.1+/BSD, and `apt-repo.sh` refuses a `~lordos` rebuild
whose `.dsc` isn't in the pool.

```sh
tools/pkg/build-wpewebkit.sh <debian-version> [lordos-revision]        # amd64 (TMPFS=64G on the 20-core machine)
ARCH=arm64 tools/pkg/build-wpewebkit.sh <debian-version> [revision]    # arm64 under qemu-user, or natively on the Pi 5
# read out/lintian-summary.txt and out/test/report.txt for both, then (W = the work folder, V = the version):
tools/release/apt-repo.sh build staging $W/amd64/out/*_amd64.deb $W/amd64/out/*_all.deb $W/arm64/out/*_arm64.deb \
    $W/amd64/out/wpewebkit_$V.dsc                                      # not the -dbgsym packages
tools/release/apt-repo.sh publish staging && tools/release/apt-repo.sh verify staging
# lord-web on the VM with the staging repository (both backends), then the same three commands with live
```

The `.dsc` goes in once (both architectures share it): `apt-repo.sh` checks every file it names (the upstream
`.orig.tar.xz` and its `.asc`, LordOS's `.debian.tar.xz`) against its SHA-256 before copying them to
`pool/main/source/w/wpewebkit/`, indexes them in `dists/stable/main/source/Sources`, and `publish` adds
`lordos-src.sources` (a `deb-src` stanza for `apt-get source`; systems keep the `deb` one the `lordos` package installs).
The pool is append-only: a changed source needs a new revision (`… <debian-version> 2`). A test of the whole path with
a throwaway key and no upload: `LORDOS_APT_OFFLINE=1 LORDOS_APT_TEST_KEYRING=<pub.gpg> LORDOS_APT_GNUPGHOME=<home>
LORDOS_APT_DIR=<scratch> tools/release/apt-repo.sh build staging …` (publish and verify refuse that key).

## A newer hardware snapshot between releases

(0.8.7; [BENCHMARK.md](BENCHMARK.md), "Newer snapshots between releases".) System Information's Compatibility panel
reads the tested-hardware snapshot from the `lordos-hwdata` package, so newly accepted reports reach installed
systems without a LordOS release: the daily update check notices one available update and Store → Updates →
*Update all* installs it; the panel reads the files again the next time it opens, with no restart. Everything
runs on the maintainer's Mac except building the `.deb` (it needs `dpkg-deb`: a build VM, or a `debian:trixie`
container):

1. **Accept the reports.** Commit them to `web/content/hardware/reports/` (and `index.json`) on `main`, and add the
   machine to [HARDWARE-REPORT.md](HARDWARE-REPORT.md) (its table, the comparison counts and "Last updated").
2. **Sign.** On the Mac: `tools/release/sign-hwsnapshot.sh`. It rebuilds `assets/hwreport/snapshot.json` from
   the reports, signs it with the release key into `snapshot.json.sig` and prints the package version: the date
   of the newest report as `YYYY.MM.DD`. If that version is already in the repository with another snapshot
   (a report was removed or corrected without a newer one), build it with `HWDATA_SERIAL=1` (then 2…), which
   makes `YYYY.MM.DD.1`, sorted after the plain date.
3. **Build** from that tree (the two files travel; the key doesn't):
   `REQUIRE_SIGNED_CODE=1 [HWDATA_SERIAL=N] tools/iso/mkdeb-hwdata.sh <folder>` prints
   `<folder>/lordos-hwdata_<version>_all.deb`. It refuses a missing or non-verifying signature and any snapshot
   format other than 1.
4. **Staging, then live** (only the new package; the pool keeps everything already published):

   ```sh
   tools/release/apt-repo.sh build staging <folder>/lordos-hwdata_<version>_all.deb
   tools/release/apt-repo.sh publish staging
   tools/release/apt-repo.sh verify staging
   # on a test VM pointed at apt-staging: Store → Check for updates lists lordos-hwdata, Update all installs it,
   # and System Information → Compatibility shows the new date in its footer
   tools/release/apt-repo.sh build live <folder>/lordos-hwdata_<version>_all.deb
   tools/release/apt-repo.sh publish live
   tools/release/apt-repo.sh verify live
   ```

5. **Changelog.** One line under `[Unreleased]` (*The tested-hardware snapshot of <date> was published: N
   reports*); no GitHub release, tag or `package.json` change. The next release carries that package again in
   its release folder (step 3 of the release), unchanged if no report arrived since.

Nothing new runs on the systems: the package engine checks `InRelease` and the file's SHA-256 as for any update,
and `lord-server` checks the snapshot's own signature before System Information sees it. A snapshot can only go
forward: going back to an older one is a downgrade, which the engine never does on its own. **The format rule:**
while any supported `lordos` reads snapshot format 1, `lordos-hwdata` carries format 1 (`mkdeb-hwdata.sh`
refuses any other); a format 2 needs `Breaks: lordos (<< <first version that reads it>)` in that script first.

A release that is pulled after publishing stays in the changelog, marked `[YANKED]`, with a line
saying why.
