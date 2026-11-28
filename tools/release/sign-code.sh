#!/bin/sh
# Signs the verified-system-code manifest (docs/SIGNING.md, "Verified system code"): regenerates
# code-manifest.json from the built bundles (themes/*/theme.bundle.js, assets/app-host.bundle.js,
# assets/sysapps/*/app.js) and writes code-manifest.sig = base64 of the Ed25519 signature over its
# exact bytes, then checks it against tools/release/lordos-release.pub.pem.
#
#   npm run build && tools/release/sign-code.sh [root] [private-key.pem]
#
# Run on the maintainer's LordOS machine, after `npm run build` and before the tree travels to the build
# VMs: the bundles are architecture-independent, so one signature serves the amd64 and arm64 .debs,
# the ISO and the Raspberry Pi image. The private key never leaves this machine. Default key:
# ~/.config/lordos-release/release-ed25519.key.pem (mode 0600). A different key (a throwaway one for
# a test build made with `make -C server CODE_PUBKEY=<hex>`) can be passed with its public key in
# CODE_PUB=<public.pem>.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
root=${1:-$repo}
key=${2:-$HOME/.config/lordos-release/release-ed25519.key.pem}
pub=${CODE_PUB:-$here/lordos-release.pub.pem}
[ -r "$key" ] || { echo "no private key at $key" >&2; exit 1; }
command -v openssl >/dev/null || { echo "openssl is needed" >&2; exit 1; }
command -v node >/dev/null || { echo "node is needed" >&2; exit 1; }
node "$repo/tools/code-manifest.mjs" --root "$root"
cd "$root"
rm -f code-manifest.sig
openssl pkeyutl -sign -rawin -inkey "$key" -in code-manifest.json | base64 | tr -d '\n' > code-manifest.sig
echo >> code-manifest.sig
base64 -d < code-manifest.sig > code-manifest.sig.bin 2>/dev/null || base64 -D < code-manifest.sig > code-manifest.sig.bin
openssl pkeyutl -verify -pubin -inkey "$pub" -rawin -in code-manifest.json -sigfile code-manifest.sig.bin >/dev/null
rm -f code-manifest.sig.bin
echo "signed code-manifest.json ($(grep -c '"path"' code-manifest.json) files) in $root, signature verified"
