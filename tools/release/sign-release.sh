#!/bin/sh
# Signs a LordOS release folder: writes SHA256SUMS (every release file) and SHA256SUMS.sig
# (base64 of the Ed25519 signature over the exact bytes of SHA256SUMS), then verifies it
# against tools/release/lordos-release.pub.pem.
#
#   tools/release/sign-release.sh <release-dir> [private-key.pem]
#
# The private key never enters the repository or a build VM. Default location:
# ~/.config/lordos-release/release-ed25519.key.pem (mode 0600) on the maintainer's machine.
set -eu
dir=${1:?usage: sign-release.sh <release-dir> [private-key.pem]}
key=${2:-$HOME/.config/lordos-release/release-ed25519.key.pem}
here=$(cd "$(dirname "$0")" && pwd)
pub=$here/lordos-release.pub.pem
[ -r "$key" ] || { echo "no private key at $key" >&2; exit 1; }
command -v openssl >/dev/null || { echo "openssl is needed" >&2; exit 1; }
cd "$dir"
rm -f SHA256SUMS SHA256SUMS.sig
files=$(find . -maxdepth 1 -type f ! -name 'SHA256SUMS*' ! -name '.*' | sed 's|^\./||' | LC_ALL=C sort)
[ -n "$files" ] || { echo "no files in $dir" >&2; exit 1; }
# the list is word-split below: only plain release file names (no spaces, globs or leading '-')
for f in $files; do
    case "$f" in -*|*[!A-Za-z0-9._+~-]*) echo "unexpected file name in $dir: $f" >&2; exit 1 ;; esac
done
# since 0.8.7 lordos depends on lordos-hwdata, and install.sh takes it only from the same release: the
# deb-only releases 0.8.8.1-0.8.8.3 shipped without it and install.sh failed on a fresh Debian (#40)
if echo "$files" | grep -q '^lordos_.*\.deb$' && ! echo "$files" | grep -q '^lordos-hwdata_.*_all\.deb$' \
    && [ "${ALLOW_NO_HWDATA:-0}" != 1 ]; then
    echo "no lordos-hwdata_<date>_all.deb in $dir: every release with lordos debs needs it (install.sh fetches it from here)." >&2
    echo "copy the current one (same bytes as the APT pool) or set ALLOW_NO_HWDATA=1 if this lordos doesn't depend on it" >&2
    exit 1
fi
# since 0.8.8.5 lordos depends on lordos-rclone (docs/CLOUD.md 3.1), one per architecture: install.sh takes it from here too
for d in $(echo "$files" | grep '^lordos_.*_[a-z0-9]*\.deb$' || true); do
    a=${d##*_}; a=${a%.deb}
    if ! echo "$files" | grep -q "^lordos-rclone_.*_$a\.deb\$" && [ "${ALLOW_NO_RCLONE:-0}" != 1 ]; then
        echo "no lordos-rclone_<version>_$a.deb in $dir for $d (tools/iso/mkdeb-rclone.sh), or ALLOW_NO_RCLONE=1 if that lordos doesn't depend on it" >&2
        exit 1
    fi
done
if command -v sha256sum >/dev/null; then sum() { sha256sum "$@"; }; else sum() { shasum -a 256 "$@"; }; fi
# shellcheck disable=SC2086
sum $files > SHA256SUMS
openssl pkeyutl -sign -rawin -inkey "$key" -in SHA256SUMS | base64 | tr -d '\n' > SHA256SUMS.sig
echo >> SHA256SUMS.sig
base64 -d < SHA256SUMS.sig > SHA256SUMS.sig.bin 2>/dev/null || base64 -D < SHA256SUMS.sig > SHA256SUMS.sig.bin
openssl pkeyutl -verify -pubin -inkey "$pub" -rawin -in SHA256SUMS -sigfile SHA256SUMS.sig.bin >/dev/null
rm -f SHA256SUMS.sig.bin
echo "signed $(wc -l < SHA256SUMS | tr -d ' ') files in $dir (SHA256SUMS + SHA256SUMS.sig), signature verified"
