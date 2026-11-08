#!/bin/sh
# build-mac.sh — the Mac download (.app and .dmg) on a Mac with Rust and Node.
#   desktop/imager/scripts/build-mac.sh          → desktop/imager/target/release/bundle/
# Both processor families. Unsigned by default; with IMAGER_SIGN_IDENTITY (a code-signing identity in
# the keychain) it is signed, notarized with the keychain profile "lordos-notary" and stapled; see
# docs/IMAGER.md, "Signing".
set -eu
cd "$(dirname "$0")/.."
npm ci --no-audit --no-fund
node build-web.mjs
rustup target add aarch64-apple-darwin x86_64-apple-darwin >/dev/null
out=target/universal-apple-darwin/release/bundle
if [ -n "${IMAGER_SIGN_IDENTITY:-}" ]; then
  APPLE_SIGNING_IDENTITY="$IMAGER_SIGN_IDENTITY" npx tauri build --target universal-apple-darwin --bundles app,dmg
  for dmg in "$out"/dmg/*.dmg; do
    xcrun notarytool submit "$dmg" --keychain-profile lordos-notary --wait
    xcrun stapler staple "$dmg"
  done
  xcrun stapler staple "$out"/macos/*.app
  spctl -a -vv "$out"/macos/*.app
else
  npx tauri build --target universal-apple-darwin --bundles app,dmg
fi
# the download's name: lordos_imager_<version>_universal.dmg
for dmg in "$out"/dmg/*.dmg; do
  v=$(cat VERSION)
  [ -n "$v" ] && mv "$dmg" "$out/dmg/lordos_imager_${v}_universal.dmg"
done
ls -la "$out"/macos "$out"/dmg
