#!/usr/bin/env bash
# All package operations and chroots run inside a rootless Fedora container.
set -euo pipefail
if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
    echo "Usage: $0 FEDORA_44_ISO LORD_DE_RPM OUTPUT_ISO [WORKDIR]" >&2
    exit 2
fi
if [ "$(id -u)" = 0 ]; then
    echo 'Run this script as your regular user, without sudo.' >&2
    exit 2
fi
repo=$(cd "$(dirname "$0")/../.." && pwd)
source_iso=$(realpath -- "$1")
desktop_rpm=$(realpath -- "$2")
output_iso=$(realpath -m -- "$3")
[ -f "$source_iso" ] && [ -f "$desktop_rpm" ]
if [ -e "$output_iso" ]; then
    echo "Output already exists: $output_iso" >&2
    exit 2
fi
mkdir -p "$repo/build" "$(dirname "$output_iso")"
if [ "$#" = 4 ]; then
    workdir=$(realpath -m -- "$4")
    mkdir -p "$workdir"
else
    workdir=$(mktemp -d "$repo/build/fedora-live.XXXXXX")
fi
if [ -e "$workdir/rootfs" ]; then
    echo "Use a fresh work directory: $workdir" >&2
    exit 2
fi
echo "Work directory: $workdir"
podman run --rm --security-opt label=disable \
    -v "$repo:/src:ro" -v "$workdir:/work:rw" \
    -v "$source_iso:/input/$(basename "$source_iso"):ro" \
    -v "$desktop_rpm:/packages/$(basename "$desktop_rpm"):ro" \
    -v "$(dirname "$output_iso"):/output:rw" \
    registry.fedoraproject.org/fedora:44 sh -ec '
        dnf install -y xorriso erofs-utils isomd5sum python3 libcap
        exec python3 /src/tools/fedora-live/remix.py "$@"
    ' sh --iso "/input/$(basename "$source_iso")" \
    --rpm "/packages/$(basename "$desktop_rpm")" --workdir /work \
    --output "/output/$(basename "$output_iso")"
