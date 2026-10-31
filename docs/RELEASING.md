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
  `lordos-0.8.0-rpi5-arm64.img.xz` and, since 0.8.7, `lordos-live-0.8.7-arm64.iso`.
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
     the arm64 ISO (0.8.7; `sudo RELEASE=1 REQUIRE_SIGNED_CODE=1 tools/iso/build-arm64.sh`, in a copy of the same
     signed tree, because it compiles `server/build` for aarch64),
     (the first release that ships it also adds arm64 to the ISO card in `web/src/home.html` and the file name in
     `web/lib/imager-guide.mjs`, and to README's downloads; they stay amd64 until the file exists)
     the Raspberry Pi 5 image (`sudo RELEASE=1 REQUIRE_SIGNED_CODE=1 tools/rpi/build-img.sh`,
     [RASPBERRY.md](RASPBERRY.md)) and the `.deb` for amd64 and arm64
     (`REQUIRE_SIGNED_CODE=1 tools/iso/mkdeb.sh`), plus (0.8.7) the one `lordos-hwdata_<date>_all.deb` for both
     (`REQUIRE_SIGNED_CODE=1 tools/iso/mkdeb-hwdata.sh <folder>`; the ISO and image builds make it too and install
     it with `lordos`, which depends on it). The package is reproducible: built again from the same snapshot and
     signature on another VM it has the same bytes. `mkdeb.sh` puts `code-manifest.json` and
     `code-manifest.sig` in `/usr/lib/lordos/` and runs `lord-server --check-code` on the staged tree:
     the package is not built if a bundle changed after signing or the signature is missing. A bundle
     rebuilt after signing means signing again. An update with a fourth number may ship without a
     new ISO (0.8.4.1 did: the 0.8.4 ISO stayed the current one, see [ISO.md](ISO.md)).
   - Copy them, and `tools/install/install.sh` as `install.sh`, into one release folder (with
     `lordos-hwdata_<date>_all.deb`: `install.sh` installs it from the release together with `lordos`, since a
     Debian system has no LordOS repository yet).
4. **SBOM.** On each build VM, `tools/release/make-sbom.sh lordos_<v>_<arch>.deb`, and put the
   `.spdx.json` files in the release folder ([COMPLIANCE.md](COMPLIANCE.md)).
5. **Sign.** On the maintainer's machine, `tools/release/sign-release.sh <release-dir>`: it writes
   `SHA256SUMS` and `SHA256SUMS.sig` and checks them against the public key
   ([SIGNING.md](SIGNING.md), "Release files"). Then copy `tools/release/lordos-release.pub.pem`
   into the folder too (it is not covered by `SHA256SUMS`: it is checked by its fingerprint).
6. **Release notes.** `tools/release/release-notes.sh <version> > notes.md` prints the
   changelog section for that version (relative links made absolute for the tag, hard-wrapped
   lines joined, its "Verifying" part replaced by the footer) plus a fixed footer: the downloads
   table, the verification steps and links to lordos.org, the paper, the docs and the security
   page. Read it once before publishing.
7. **The APT repository** (what installed systems update from; [UPDATES.md](UPDATES.md)). On the
   maintainer's Mac, where the archive key lives (never on a VM):

   ```sh
   # staging first: the same .debs, the notes from this version's CHANGELOG section
   tools/release/apt-repo.sh build staging <release-dir>/lordos-hwdata_<date>_all.deb <release-dir>/lordos_<file-version>_amd64.deb <release-dir>/lordos_<file-version>_arm64.deb
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
   tools/release/apt-repo.sh build live <release-dir>/lordos-hwdata_<date>_all.deb <release-dir>/lordos_<file-version>_amd64.deb <release-dir>/lordos_<file-version>_arm64.deb
   tools/release/apt-repo.sh publish live
   tools/release/apt-repo.sh verify live
   ```

   A version is never re-uploaded with other contents (the script refuses); a fix is a new version.
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
10. **Check:** download from the release page and run the verification commands from the notes;
   run `curl -fsSL https://lordos.org/install.sh | sh` on a clean Debian 13; update the website if
   it names the current version.

## A newer hardware snapshot between releases

(0.8.7; [BENCHMARK.md](BENCHMARK.md), "Newer snapshots between releases".) System Information's Compatibility panel
reads the tested-hardware snapshot from the `lordos-hwdata` package, so newly accepted reports reach installed
systems without a LordOS release: the daily update check notices one available update and Store → Updates →
*Update all* installs it; the panel reads the files again the next time it opens, with no restart. Everything
runs on the maintainer's Mac except building the `.deb` (it needs `dpkg-deb`: a build VM, or a `debian:trixie`
container):

1. **Accept the reports.** Commit them to `web/content/hardware/reports/` (and `index.json`) on `main`.
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
