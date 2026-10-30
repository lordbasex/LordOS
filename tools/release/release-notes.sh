#!/bin/sh
# Prints the GitHub release body for one LordOS version: the matching section of CHANGELOG.md
# (without its heading or its "### Verifying" part), relative links made absolute for that tag, and a fixed footer with the
# downloads table, the verification steps and links.
#
#   tools/release/release-notes.sh <version> > notes.md
#   gh release create v<version> --title "LordOS <version>" --prerelease --notes-file notes.md ...
#
# <version> is the CHANGELOG version, with or without a leading "v" (e.g. 0.8.0-beta).
# Environment:
#   LORDOS_FILE_VERSION  the version in the file names (default: <version> without its
#                        pre-release suffix, e.g. 0.8.0, which is package.json's "version")
#   LORDOS_CHANGELOG     the changelog to read (default: CHANGELOG.md at the repository root)
# See docs/RELEASING.md.
set -eu

usage() { echo "usage: release-notes.sh <version>   (e.g. 0.8.0-beta)" >&2; exit 2; }
[ $# -eq 1 ] || usage
version=${1#v}
case "$version" in
    "" | *[!A-Za-z0-9.+-]*) echo "release-notes.sh: invalid version: $1" >&2; exit 2 ;;
esac

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
changelog=${LORDOS_CHANGELOG:-$root/CHANGELOG.md}
[ -r "$changelog" ] || { echo "release-notes.sh: cannot read $changelog" >&2; exit 1; }

tag=v$version
filever=${LORDOS_FILE_VERSION:-${version%%-*}}
repo=https://github.com/lordbasex/LordOS
blob=$repo/blob/$tag

# The section: from "## [<version>]" up to the next "## [" heading or the link references.
section=$(awk -v v="$version" '
    BEGIN { head = "## [" v "]" }
    index($0, head) == 1 && (length($0) == length(head) || substr($0, length(head) + 1, 1) == " ") {
        found = 1; next
    }
    found && (/^## \[/ || /^\[[^]]+\]: /) { exit }
    # the footer carries the verification steps, so the "### Verifying" part is dropped
    found && /^### / { skip = ($0 ~ /^### Verifying/) }
    found && !skip { print }
' "$changelog")

if [ -z "$(printf '%s' "$section" | tr -d '[:space:]')" ]; then
    echo "release-notes.sh: no \"## [$version]\" section in $changelog" >&2
    exit 1
fi

# Relative links (docs/…, tools/…, SECURITY.md…) point at the tag's files on GitHub. GitHub
# renders every newline of a release body as a line break, so the changelog's hard-wrapped
# paragraphs and list items are joined into single lines (code blocks, headings and tables are
# left alone). Blank lines at both ends are trimmed.
printf '%s\n' "$section" |
    sed -E "s#\]\(((docs|tools|web|server|apps|sdk|themes|runtime|packaging|tests|examples)/[^)]*|[A-Za-z_-]+\.md(\#[^)]*)?)\)#](${blob}/\1)#g" |
    awk '
        function flush() { if (buf != "") { out[++n] = buf; buf = "" } }
        function emit(s) { flush(); out[++n] = s }
        /^[[:space:]]*```/ { code = !code; emit($0); next }
        code { emit($0); next }
        /^[[:space:]]*$/ { emit(""); next }
        /^(#|---)|^[[:space:]]*\|/ { emit($0); next }
        /^[[:space:]]*([-*+]|[0-9]+\.)[[:space:]]/ { flush(); buf = $0; next }
        {
            if (buf == "") { buf = $0; next }
            line = $0; sub(/^[[:space:]]+/, "", line); buf = buf " " line
        }
        END {
            flush()
            first = 1; while (first <= n && out[first] == "") first++
            while (n >= first && out[n] == "") n--
            for (i = first; i <= n; i++) print out[i]
        }'

cat <<EOF

---

## Downloads

| File | What it is |
|---|---|
| \`lordos-live-${filever}-amd64.iso\` | Live and installable image for 64-bit PCs (UEFI and BIOS). Write it to a USB stick as is; Secure Boot off. |
| \`lordos-${filever}-rpi5-arm64.img.xz\` | Image for the Raspberry Pi 5, for the LordOS Imager (below) or Raspberry Pi Imager ("Use custom"), an SD card or an NVMe drive. |
| \`lordos_${filever}_amd64.deb\` | The LordOS package for an existing Debian 13 on a PC (installed by \`install.sh\`, or with \`apt install ./…\`). |
| \`lordos_${filever}_arm64.deb\` | The same package for Debian 13 on arm64. |
| \`lordos_imager_${filever}_universal.dmg\` | LordOS Imager for Mac (both processor families, signed and notarized): writes LordOS to an SD card or a USB stick and prepares the Raspberry Pi 5's first boot. |
| \`lordos_imager_${filever}_x64_setup.exe\` | LordOS Imager for Windows (64-bit installer; not code-signed yet, so the system asks before running it). |
| \`lordos_imager_${filever}_amd64.deb\` | LordOS Imager for Linux (Debian 13 and similar, amd64; needs udisks2). |
| \`install.sh\` | The one-line installer for Debian 13: \`curl -fsSL https://lordos.org/install.sh \| sh\`. It checks the signature before installing. |
| \`SHA256SUMS\` | The SHA-256 of every file in this release. |
| \`SHA256SUMS.sig\` | The Ed25519 signature of \`SHA256SUMS\` with the LordOS release key (base64). |
| \`lordos_${filever}_<arch>.spdx.json\` | Software bill of materials (SPDX 2.3) for each package: what it bundles and the Debian packages it depends on. |

## Verifying the downloads

The LordOS release public key is \`lordos-release.pub.pem\` ([in the repository](${blob}/tools/release/lordos-release.pub.pem), or at https://lordos.org/lordos-release.pub.pem). Its fingerprint, the SHA-256 of the DER public key, is:

\`\`\`
c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e
\`\`\`

With OpenSSL 3, in the folder with the downloads:

\`\`\`sh
openssl pkey -pubin -in lordos-release.pub.pem -outform DER | sha256sum   # must match the fingerprint
base64 -d SHA256SUMS.sig > SHA256SUMS.sig.bin
openssl pkeyutl -verify -pubin -inkey lordos-release.pub.pem -rawin -in SHA256SUMS -sigfile SHA256SUMS.sig.bin
sha256sum -c --ignore-missing SHA256SUMS
\`\`\`

"Signature Verified Successfully" and "OK" for each downloaded file mean the files are the ones we published. Details: [docs/SIGNING.md](${blob}/docs/SIGNING.md).

## Links

- Website: [lordos.org](https://lordos.org)
- The project paper: [lordos.org/paper](https://lordos.org/paper/)
- Documentation: [docs/README.md](${blob}/docs/README.md)
- Security policy and reporting a vulnerability: [lordos.org/legal/security](https://lordos.org/legal/security/)
- Full changelog: [CHANGELOG.md](${blob}/CHANGELOG.md)
EOF
