#!/bin/sh
# build-windows.sh — the Windows download (an NSIS installer and the .exe), cross-built with
# cargo-xwin in a Debian 13 container. The .exe carries the administrator manifest
# (src-tauri/windows-app.manifest).
#   desktop/imager/scripts/build-windows.sh      → desktop/imager/target/windows/
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
docker build -t lordos-imager-build -f "$here/Dockerfile.build" "$here"
docker run --rm -v "$repo:/src" -v lordos-imager-cargo:/opt/cargo/registry -v lordos-imager-xwin:/root/.cache/cargo-xwin \
  -v lordos-imager-nm-linux:/src/desktop/imager/node_modules -v lordos-imager-target-windows:/build \
  -e CARGO_TARGET_DIR=/build -w /src/desktop/imager lordos-imager-build sh -c '
    set -e
    npm ci --no-audit --no-fund && node build-web.mjs
    npx tauri build --runner cargo-xwin --target x86_64-pc-windows-msvc --bundles nsis
    mkdir -p target/windows && for f in /build/x86_64-pc-windows-msvc/release/bundle/nsis/*.exe; do v=$(cat VERSION); cp "$f" "target/windows/lordos_imager_${v}_x64_setup.exe"; cp /build/x86_64-pc-windows-msvc/release/lordos-imager.exe "target/windows/lordos_imager_${v}_x64.exe"; done
    ls -la target/windows/'
