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
  `lordos-0.8.0-rpi5-arm64.img.xz`.
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
     (`assets/hwreport/snapshot.json.sig`, same key and format); `REQUIRE_SIGNED_CODE=1 tools/iso/mkdeb.sh` refuses
     an unsigned one.
   - Send the tree, with both files, to the build VMs (never the key). Build lord-server as a release:
     `make -C server RELEASE=1` (it only runs code signed with the release key, with no development
     mode).
   - Build the ISO on the build VM (`sudo REQUIRE_SIGNED_CODE=1 tools/iso/build.sh`, [ISO.md](ISO.md)),
     the Raspberry Pi 5 image (`sudo RELEASE=1 REQUIRE_SIGNED_CODE=1 tools/rpi/build-img.sh`,
     [RASPBERRY.md](RASPBERRY.md)) and the `.deb` for amd64 and arm64
     (`REQUIRE_SIGNED_CODE=1 tools/iso/mkdeb.sh`). `mkdeb.sh` puts `code-manifest.json` and
     `code-manifest.sig` in `/usr/lib/lordos/` and runs `lord-server --check-code` on the staged tree:
     the package is not built if a bundle changed after signing or the signature is missing. A bundle
     rebuilt after signing means signing again. An update with a fourth number may ship without a
     new ISO (0.8.4.1 did: the 0.8.4 ISO stayed the current one, see [ISO.md](ISO.md)).
   - Copy them, and `tools/install/install.sh` as `install.sh`, into one release folder.
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
   tools/release/apt-repo.sh build staging <release-dir>/lordos_<file-version>_amd64.deb <release-dir>/lordos_<file-version>_arm64.deb
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
   tools/release/apt-repo.sh build live <release-dir>/lordos_<file-version>_amd64.deb <release-dir>/lordos_<file-version>_arm64.deb
   tools/release/apt-repo.sh publish live
   tools/release/apt-repo.sh verify live
   ```

   A version is never re-uploaded with other contents (the script refuses); a fix is a new version.
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

A release that is pulled after publishing stays in the changelog, marked `[YANKED]`, with a line
saying why.
