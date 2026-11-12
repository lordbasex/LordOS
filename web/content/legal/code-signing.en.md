LordOS signs what it publishes so that you can tell a file comes from the project and was not changed on the way. This page explains what is signed, with which keys, and how to check it yourself. For reporting a problem with a signature, see the [Security policy](/legal/security/).

## Release files

Every file of a GitHub release (the ISO image, the Raspberry Pi 5 image and the `.deb` package) is listed with its SHA-256 in a file called `SHA256SUMS`. `SHA256SUMS` is signed with the **LordOS release key**:

- `SHA256SUMS.sig` is the base64 of the 64-byte **Ed25519** signature over the exact bytes of `SHA256SUMS`.
- Public key: [lordos-release.pub.pem](/lordos-release.pub.pem), also in the repository as [`tools/release/lordos-release.pub.pem`](https://github.com/lordbasex/LordOS/blob/main/tools/release/lordos-release.pub.pem).
- Fingerprint (SHA-256 of the DER public key):
  `c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e`

The Debian installer script (`lordos.org/install.sh`) carries the same public key inside it. It checks the signature before downloading the `.deb`, then checks the `.deb` against `SHA256SUMS`, and refuses to install if either check fails.

To check a download by hand, with OpenSSL 3, in the folder where you saved the files:

```
base64 -d SHA256SUMS.sig > SHA256SUMS.sig.bin
openssl pkeyutl -verify -pubin -inkey lordos-release.pub.pem -rawin -in SHA256SUMS -sigfile SHA256SUMS.sig.bin
sha256sum -c --ignore-missing SHA256SUMS
```

The first command decodes the signature, the second must print `Signature Verified Successfully`, and the third must print `OK` for each file you downloaded. To check the key itself, compare its fingerprint with the one above:

```
openssl pkey -pubin -in lordos-release.pub.pem -outform DER | sha256sum
```

## LordOS apps (.lpk) and the Store

LordOS native apps come as `.lpk` packages. A signed `.lpk` carries the list of all its files with their SHA-256 and an **Ed25519** signature over that list. LordOS verifies it in its own C code when the app is installed and every time it is loaded, so a changed file is detected.

The Store and the install dialog show one of these trust levels:

| Level | When |
|---|---|
| **LordOS** | signed with the official LordOS key, which is compiled into the system (no file in your home folder can add an official key) |
| **Verified author** | signed with a key listed in the signed LordOS catalog |
| **Signed by <name>** | signed with a key LordOS doesn't know |
| **Unsigned** | no signature: the Store warns you to install it only from people you trust |

More rules protect you after installation:

- **Same key for updates.** A new version must be signed with the same key as the installed one; an update signed with another key is rejected.
- **Signed catalog and revocation.** The LordOS catalog is signed with the LordOS key and carries the keys of verified authors and a **revocation list** of keys and packages. A revoked package is not installed, and an installed one is disabled with a clear message.
- **Modified by you.** If you edit an installed app, LordOS marks it as modified, shows which files changed and can restore the original. If you share your version, it is signed with your own key and credits the original author.

## Debian packages

The base system and every package in the Store's Debian section come from Debian and are protected by Debian's own signatures:

- The package index (`InRelease`) must carry a valid OpenPGP signature from a key in the source's keyring, installed by Debian's `debian-archive-keyring` package. Expired, revoked or replayed indexes are rejected.
- Every package list and every `.deb` is checked against the SHA-256 in that signed index before it is stored or passed to `dpkg`.
- LordOS does these checks with its own package engine, and `apt` keeps working alongside it with the same keys.

Downloads use HTTPS when the source's address is `https://` (never downgraded to HTTP, even by a redirect), and HTTP otherwise, as with `apt` and `deb.debian.org`: security comes from the signatures and hashes, so on plain HTTP someone on the network can see which packages you download but cannot change them.

## LordOS's update repository

Installed systems get LordOS's own updates from LordOS's APT repository at `lordos.org/apt`. Its index is signed with the **LordOS archive key**, an OpenPGP Ed25519 key separate from the release key, because APT needs OpenPGP:

- Public key: [`packaging/keys/lordos-archive-keyring.asc`](https://github.com/lordbasex/LordOS/blob/main/packaging/keys/lordos-archive-keyring.asc) in the repository, installed on every system as `/usr/share/keyrings/lordos-archive-keyring.gpg` by the `lordos` package.
- Fingerprint: `1DDA F390 107D F264 FA8A  0918 3972 CC6D E4DA 2336`
- The repository's source is limited to that key (`Signed-By`), and its index and packages are checked the same way as Debian's above.

To check the key by hand: `gpg --show-keys /usr/share/keyrings/lordos-archive-keyring.gpg` must show the fingerprint above.

## LordOS's own code

The desktop, the login screen and every native app are JavaScript bundles. Their sizes and SHA-256 are listed in `code-manifest.json`, signed with the **release key** above. Since 0.8.2 `lord-server`, which has that public key compiled in, checks the signature and each bundle's SHA-256 before it runs the bundle. A bundle that doesn't match is not run: About shows **Verified** for a signed system, **Development build** for an unsigned one, and **Modified — not loaded** when something doesn't match, and LordOS shows a recovery screen instead of a blank one when nothing is left to load.

## Raspberry Pi archive key

## Raspberry Pi archive key

On the Raspberry Pi 5, the kernel and firmware come from the Raspberry Pi archive (`archive.raspberrypi.com`), and everything else from Debian:

- The image build takes the archive key from the archive's `raspberrypi-archive-keyring` package and checks its **fingerprint** against the one we verified (`CF8A1AF502A2AA2D763BAE7E82B129927FA3303E`); the build stops if they differ.
- The key is installed only for that source (`signed-by=/etc/apt/keyrings/raspberrypi.pgp`), so it can't vouch for packages from anywhere else.
- An apt **pin** gives the archive a low priority, except for the kernel, the firmware and their utilities: no other package can be replaced from it.

## How we keep the keys

- The **release key**'s private half lives only on the maintainer's computer, readable only by its owner, with an offline backup. It is never stored in the repository, on a build machine or on a server.
- The private half of the **LordOS key** for `.lpk` packages is also kept outside the repository, on the maintainer's computer. Today it is a development key; the final key for public releases will replace it in the system and the change will be announced.
- The private half of the **LordOS archive key** is also kept only on the maintainer's computer, with a backup copy.
- Public keys are published in the repository and on this website, so anyone can check them.

## If a key is compromised

If we ever suspect that a private key was exposed, we will:

1. stop using it and **revoke** it: for `.lpk` packages, through the revocation list of the signed catalog; for releases, by removing it from the installer script;
2. create a **new key** and announce its fingerprint on lordos.org (on this page and the [Security policy](/legal/security/)) and on GitHub;
3. **re-sign** the affected releases and packages with the new key, and publish an advisory explaining what happened and what you should check.

A fingerprint that doesn't match the one on this page means you should not install the file. Tell us through [our contact form](https://www.iperfex.com/contacto/?lang=en), with "Security" in the subject.
