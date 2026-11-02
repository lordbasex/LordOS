# apt/ — the curated app catalog

`apt/catalog.json` lists the third-party apps LordOS offers through its Store with the normal package
engine. It is the only source of truth: nothing goes into the repository unless it is pinned here, and
every change is reviewed in a pull request like code. See `docs/UPDATES.md`, "Mirrored apps
(catalog)", for how it fits into the signed repository on lordos.org.

```
apt/catalog.json        the apps (schema 1, below)
apt/keys/<vendor>.asc   the vendors' APT signing keys (public), pinned by fingerprint in the catalog
apt/recipes/<recipe>/   how LordOS builds its own .deb of an app that has no vendor .deb
apt/recipes/lib.sh      helpers shared by the recipes
apt/build.sh            check | fetch | list   (POSIX sh; jq, gpg/gpgv, curl and docker on the Mac)
```

## Two ways an app reaches a system

- **Direct** (`"delivery": "direct"`): the vendor has its own signed APT repository (Claude Code,
  Claude Desktop, Visual Studio Code, Cursor, Google Chrome, Microsoft Edge, Spotify). The user's
  system adds that repository itself (a
  deb822 `.sources` file with the pinned key from `apt/keys/`) and installs and updates the package
  from the vendor. Nothing is copied to lordos.org. `apt/build.sh check` still verifies the entry.
- **Mirrored** (everything else, and an `apt` entry with `"delivery": "mirror"`): the files are
  downloaded on the maintainer's Mac, verified, packaged when they are not a .deb, and added to
  `pool/catalog/` of lordos.org/apt by `tools/release/apt-repo.sh build <target> --catalog …`, signed
  with the LordOS archive key together with everything else. Today: llama.cpp, whisper.cpp and codex-cli (built by LordOS) and goose and opencode (the vendor's .deb).

## Schema (version 1)

```jsonc
{
  "schema": 1,
  "apps": [{
    "id": "llama.cpp",                 // unique, [a-z0-9][a-z0-9.+-]+
    "package": "llama.cpp",            // the Debian package users install
    "extra_packages": [],              // optional: other packages a recipe may produce
    "name": "llama.cpp",
    "summary": { "en": "…", "es": "…", "pt": "…" },   // one short neutral line per language
    "license": "MIT", "vendor": "ggml.ai (ggml-org)", "homepage": "https://github.com/ggml-org/llama.cpp",
    "arches": ["amd64", "arm64"],
    "kind": "local-models",            // coding-agent | editor | local-models | chat | speech | browser | music | …
    "section": "commercial",           // optional: "commercial" = free of charge but proprietary, no source
                                       // (Chrome, Edge, Spotify); missing = the AI section
    "source": { "type": "…", … }       // one of the four below
  }]
}
```

`source` by `type`:

| type | fields | versions come from |
|---|---|---|
| `apt` | `delivery` (`direct`/`mirror`), `url`, `suite` (the `dists/` name: the Suite, or the Codename when Suite is empty, as Cursor's), `components` [], `arches` [], `key` (`keys/<name>.asc`), `fingerprint` (40 hex digits, the key's primary fingerprint), `package`, optional `files` (`sources`, `keyring`: the vendor's own file names, for a package that always writes its own source, as Cursor's; `docs/UPDATES.md`) | the vendor's signed index: the newest version per architecture |
| `deb` | `version`, `files.<arch>.{url, sha256}` | the catalog |
| `build` | `recipe`, `version`, `revision`, `source_date_epoch`, `tag`, `commit`, `url` + `sha256` of the source tarball, `inputs` [{`name`, `url`, `sha256`}], `notes.<arch>` | the catalog |
| `binary` | `recipe`, `version`, `revision`, `source_date_epoch`, `files.<arch>.{url, sha256, sigstore?}`, `sigstore.{identity, issuer}`, `inputs` [] | the catalog |

A package LordOS builds is versioned `<version>-<revision>` (`0.7.0-1lordos1`). A `deb822` source a
system writes for a direct entry is exactly:

```
Types: deb
URIs: <url>
Suites: <suite>
Components: <components…>
Architectures: <arches…>
Signed-By: <the key from apt/keys/, fingerprint <fingerprint>>
```

## Updating an app

1. Mirrored or built app: change `version` (and `revision`, back to `1lordos1` for a new upstream
   version) and every `sha256`, `url`, `tag`/`commit` and `source_date_epoch` in the same entry. Take
   the hashes from the vendor's own published digests (a GitHub release asset's `digest`, a
   `sha256sum.txt`), then confirm them on a second download. For codex-cli, also the sigstore bundle
   and the identity (`…/rust-release.yml@refs/tags/rust-v<version>`).
2. A LordOS packaging change without a new upstream version: bump `revision` (`1lordos2`). A
   published version is never replaced: the pool is append-only, and `apt-repo.sh` refuses a file
   with the same name and different contents. Built packages are kept in the cache and not rebuilt.
3. Direct or `apt` app: nothing to bump; the newest version in the vendor's signed index is used.
   Only a key rotation changes the entry (a new `apt/keys/*.asc` and `fingerprint`, reviewed).
4. Run `apt/build.sh check` (and `apt/build.sh fetch <id>` for a mirrored app), then open the pull
   request with the output. The repository is built and signed only on the maintainer's Mac.

## Trust rules

- **Pinned, not discovered.** Every byte that reaches the pool matches a SHA-256 written in this
  file, or (for `apt` sources) the SHA-256 of a vendor index whose `InRelease` verifies with the
  pinned key: `gpgv` with only that key, a `VALIDSIG` by the pinned fingerprint, the key file holding
  exactly one primary key, the suite and `Valid-Until` checked, the `Packages` file checked against
  `InRelease`, the `.deb` checked against `Packages` (SHA-256 and size), its `Filename` inside
  `pool/` with no `..`, and its control fields (`Package`, `Version`, `Architecture`) checked.
- **Downloads are https only** (redirects too), and only the URLs in the catalog.
- **Builds have no network.** Recipes run in a `debian:trixie` container of the target architecture
  (`--platform linux/amd64` or `linux/arm64`, under qemu on the Mac) with `--network none`, as the
  invoking user, with the verified inputs read-only. Anything upstream's build would download (llama.cpp's
  web UI, whisper.cpp's `talk-llama`) is either a pinned input or left out.
- **Signatures when the vendor has them.** codex-cli's binary is checked against its sigstore bundle
  with `cosign verify-blob` (keyless: the GitHub Actions workflow identity of that tag and the
  `token.actions.githubusercontent.com` issuer), with a local `cosign` or the pinned cosign container.
- **Vendor .debs are never modified**, including their maintainer scripts (`docs/UPDATES.md` lists
  what they do when the Store installs them).
- **Cached, so it can be republished.** `$WORKDIR/catalog-cache` (`~/.local/share/lordos-apt`) keeps
  every verified download and every built package; a version that vanished upstream can still be
  published, and a built package is never rebuilt for the same version.
