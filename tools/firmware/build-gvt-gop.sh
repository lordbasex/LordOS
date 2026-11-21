#!/usr/bin/env bash
# Builds LordOS's own UEFI GOP option ROM for Intel GVT-g vGPUs (firmware/gvt-gop) against a pinned EDK2.
#
#   tools/firmware/build-gvt-gop.sh [--out DIR] [--work DIR] [--device-id 0x9BC5]
#
# Fetches edk2-stable202502 (tarball SHA-256 and embedded commit checked), builds the BaseTools it needs
# (GenFw, EfiRom), builds the driver X64 DEBUG and RELEASE, and wraps each with EfiRom as a PCI option ROM for
# vendor 0x8086 and the given device ID (Comet Lake GT2 by default). Prints the SHA-256 of every output.
# Needs: gcc, make, nasm, python3, uuid-dev, curl, git (for get-tar-commit-id). Nothing here runs as root.
# Signing for a Secure Boot db (sbsign) is a separate step: sign GvtGopDxe-release.efi, then wrap it again (README).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/firmware/gvt-gop/out"
WORK="${XDG_CACHE_HOME:-$HOME/.cache}/lordos-firmware"
DEVICE_ID=0x9BC5

EDK2_TAG=edk2-stable202502
EDK2_COMMIT=fbe0805b2091393406952e84724188f8c1941837
EDK2_URL="https://github.com/tianocore/edk2/archive/refs/tags/$EDK2_TAG.tar.gz"
EDK2_SHA256=07313bb66950328d2e156f79128bc0b2bbec6428837b130bee14c199671f5f78

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --work) WORK="$2"; shift 2 ;;
    --device-id) DEVICE_ID="$2"; shift 2 ;;
    *) echo "usage: $0 [--out DIR] [--work DIR] [--device-id 0xNNNN]" >&2; exit 2 ;;
  esac
done
[[ "$DEVICE_ID" =~ ^0x[0-9A-Fa-f]{4}$ ]] || { echo "bad --device-id: $DEVICE_ID" >&2; exit 2; }

mkdir -p "$WORK" "$OUT"
WORK="$(cd "$WORK" && pwd)"
OUT="$(cd "$OUT" && pwd)"
TARBALL="$WORK/$EDK2_TAG.tar.gz"
EDK2="$WORK/edk2-$EDK2_TAG"

# 1. EDK2 at the pinned tag.
if [ ! -f "$TARBALL" ] || ! echo "$EDK2_SHA256  $TARBALL" | sha256sum -c --quiet - 2>/dev/null; then
  echo "fetching $EDK2_URL"
  curl -fsSL --retry 3 -o "$TARBALL.part" "$EDK2_URL"
  mv "$TARBALL.part" "$TARBALL"
fi
echo "$EDK2_SHA256  $TARBALL" | sha256sum -c --quiet - || { echo "EDK2 tarball hash mismatch" >&2; exit 1; }
COMMIT="$(set +o pipefail; gzip -dc "$TARBALL" 2>/dev/null | git get-tar-commit-id)"
[ "$COMMIT" = "$EDK2_COMMIT" ] || { echo "EDK2 tarball is commit $COMMIT, expected $EDK2_COMMIT" >&2; exit 1; }

if [ ! -f "$EDK2/.lordos-extracted" ]; then
  rm -rf "$EDK2" "$WORK/extract"
  mkdir -p "$WORK/extract"
  tar -xzf "$TARBALL" -C "$WORK/extract"
  mv "$WORK/extract/edk2-$EDK2_TAG" "$EDK2"
  rmdir "$WORK/extract"
  # The package .dec files list include folders that live in git submodules (MIPI SyS-T, Brotli, libfdt),
  # which release tarballs lack. Nothing this build compiles uses them; empty folders satisfy the parser.
  for dec in MdePkg/MdePkg.dec MdeModulePkg/MdeModulePkg.dec OvmfPkg/OvmfPkg.dec; do
    tr -d '\r' < "$EDK2/$dec" | awk '/^\[/ { inc = ($0 ~ /^\[Includes/); next } inc && $1 !~ /^#/ && NF { print $1 }' |
      while read -r inc; do mkdir -p "$EDK2/$(dirname "$dec")/$inc"; done
  done
  touch "$EDK2/.lordos-extracted"
fi

# 2. Only the BaseTools this build uses (the full set needs git submodules the tarball lacks).
if [ ! -x "$EDK2/BaseTools/Source/C/bin/GenFw" ] || [ ! -x "$EDK2/BaseTools/Source/C/bin/EfiRom" ]; then
  make -s -C "$EDK2/BaseTools/Source/C" makerootdir Common GenFw EfiRom -j"$(nproc)"
fi

# 3. The driver, DEBUG and RELEASE, built from a copy of the package next to EDK2.
rm -rf "$WORK/pkgs"
mkdir -p "$WORK/pkgs"
cp -a "$ROOT/firmware/gvt-gop/GvtGopPkg" "$WORK/pkgs/"

build_target() {
  local target="$1"
  (
    cd "$EDK2"
    export WORKSPACE="$WORK/ws"
    export PACKAGES_PATH="$EDK2:$WORK/pkgs"
    export EDK_TOOLS_PATH="$EDK2/BaseTools"
    export SOURCE_DATE_EPOCH=0
    mkdir -p "$WORKSPACE"
    # shellcheck disable=SC1091
    set +u; . "$EDK2/edksetup.sh" BaseTools >/dev/null; set -u
    build -q -a X64 -t GCC5 -b "$target" -p GvtGopPkg/GvtGopPkg.dsc -n "$(nproc)"
  )
  local efi="$WORK/ws/Build/GvtGopPkg/${target}_GCC5/X64/GvtGopDxe.efi"
  local tag; tag="$(echo "$target" | tr 'A-Z' 'a-z')"
  # Zero the debug directory (it names the build path) and time stamps, so the output depends only on the
  # sources and EDK2, not on where --work is.
  "$EDK2/BaseTools/Source/C/bin/GenFw" -z -o "$OUT/GvtGopDxe-$tag.efi" "$efi" >/dev/null
  "$EDK2/BaseTools/Source/C/bin/EfiRom" -f 0x8086 -i "$DEVICE_ID" -l 0x030000 -ec "$OUT/GvtGopDxe-$tag.efi" \
    -o "$OUT/lordos-gvt-gop-$tag.rom" >/dev/null
}

build_target RELEASE
build_target DEBUG

cd "$OUT"
sha256sum GvtGopDxe-release.efi lordos-gvt-gop-release.rom GvtGopDxe-debug.efi lordos-gvt-gop-debug.rom | tee SHA256SUMS
