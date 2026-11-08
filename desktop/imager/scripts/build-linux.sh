#!/bin/sh
# build-linux.sh — the Linux download (.deb and the bare binary) in a Debian 13 container.
#   desktop/imager/scripts/build-linux.sh        → desktop/imager/target/linux/
# The container keeps its own node_modules and cargo target folder (named volumes), so the host's
# (another platform's) are never mixed in; the results are copied out at the end.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
docker build -t lordos-imager-build -f "$here/Dockerfile.build" "$here"
docker run --rm -v "$repo:/src" -v lordos-imager-cargo:/opt/cargo/registry \
  -v lordos-imager-nm-linux:/src/desktop/imager/node_modules -v lordos-imager-target-linux:/build \
  -e CARGO_TARGET_DIR=/build -w /src/desktop/imager lordos-imager-build sh -c '
    set -e
    npm ci --no-audit --no-fund && node build-web.mjs
    npx tauri build --bundles deb
    mkdir -p target/linux && cp /build/release/lordos-imager target/linux/ && for f in /build/release/bundle/deb/*.deb; do v=$(cat VERSION); cp "$f" "target/linux/lordos_imager_${v}_amd64.deb"; done
    ls -la target/linux/'
