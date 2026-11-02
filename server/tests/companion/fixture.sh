#!/bin/bash
# fixture.sh <output folder> — on the development machine (Node, Go): a signed test package and catalogs for
# tests/companion_e2e.sh, with throwaway keys that exist only there. Never the release key.
set -e
OUT=$1; ROOT=$(cd "$(dirname "$0")/../../.." && pwd); L="node $ROOT/tools/lpk.mjs"
rm -rf "$OUT"; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
(cd "$ROOT/server/tests/companion/probe" && CGO_ENABLED=0 GOOS=linux GOARCH=${GOARCH:-amd64} go build -trimpath -ldflags='-s -w' -o "$OUT/engine" .)
$L keygen --name Official --out "$OUT/official.json" >/dev/null
$L keygen --name Author --out "$OUT/author.json" >/dev/null
cd "$OUT"
$L tree "$ROOT/server/tests/companion/app" pkgtest --key author.json --companion engine
pub() { node -e "console.log(JSON.parse(require('fs').readFileSync('$1')).public)"; }
A=$(pub author.json); pub official.json > official.hex
cat > src-good.json <<X
{ "serial": 10, "authors": [{ "key": "$A", "name": "Test author" }] }
X
cat > src-none.json <<X
{ "serial": 10, "authors": [] }
X
cat > src-revoked.json <<X
{ "serial": 11, "authors": [{ "key": "$A", "name": "Test author" }], "revoked": { "keys": ["$A"] } }
X
for c in good none revoked; do $L catalog src-$c.json --key official.json cat-$c >/dev/null; done
rm -f official.json author.json src-*.json engine
echo "$OUT ready"
