#!/bin/sh
# Signs the tested-hardware catalogue's snapshot (docs/BENCHMARK.md, "0.8.7 — Community hardware reports"):
# rebuilds assets/hwreport/snapshot.json from the accepted reports (web/content/hardware/reports, through
# tools/hwreport/catalog.mjs, which validates every report against the schema first) and writes
# assets/hwreport/snapshot.json.sig = base64 of the Ed25519 signature over its exact bytes, then checks it
# against tools/release/lordos-release.pub.pem. lord-server verifies it in C (server/src/sysreport/hwsnap.c)
# with the key compiled in before System Information's Compatibility panel sees any of it.
#
#   tools/release/sign-hwsnapshot.sh [root] [private-key.pem]
#
# Run on the maintainer's machine only, next to tools/release/sign-code.sh and before the tree travels to the
# build VMs: the snapshot is the same for every architecture and ships in the lordos package (tools/iso/mkdeb.sh),
# so it updates through the signed APT repository. The private key never leaves this machine. Default key:
# ~/.config/lordos-release/release-ed25519.key.pem (mode 0600). A throwaway key for a test build made with
# `make -C server CODE_PUBKEY=<hex>` can be passed with its public key in CODE_PUB=<public.pem>.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
root=${1:-$repo}
key=${2:-$HOME/.config/lordos-release/release-ed25519.key.pem}
pub=${CODE_PUB:-$here/lordos-release.pub.pem}
[ -r "$key" ] || { echo "no private key at $key" >&2; exit 1; }
command -v openssl >/dev/null || { echo "openssl is needed" >&2; exit 1; }
command -v node >/dev/null || { echo "node is needed" >&2; exit 1; }
node "$repo/tools/hwreport/catalog.mjs" --reports "$root/web/content/hardware/reports" --snapshot "$root/assets/hwreport/snapshot.json"
cd "$root/assets/hwreport"
rm -f snapshot.json.sig
openssl pkeyutl -sign -rawin -inkey "$key" -in snapshot.json | base64 | tr -d '\n' > snapshot.json.sig
echo >> snapshot.json.sig
base64 -d < snapshot.json.sig > snapshot.json.sig.bin 2>/dev/null || base64 -D < snapshot.json.sig > snapshot.json.sig.bin
openssl pkeyutl -verify -pubin -inkey "$pub" -rawin -in snapshot.json -sigfile snapshot.json.sig.bin >/dev/null
rm -f snapshot.json.sig.bin
echo "signed assets/hwreport/snapshot.json ($(wc -c < snapshot.json | tr -d ' ') bytes) in $root, signature verified"
