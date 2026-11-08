#!/bin/sh
# fetch-audiocpp.sh — audio.cpp (Apache-2.0, github.com/0xShug0/audio.cpp) at the one commit LordOS was tested with,
# into third_party/audiocpp (ignored by git). Only lord-tts links it (docs/VOICE.md); server/mk/tts.mk builds just its
# Pocket TTS family, CPU and Vulkan. The commit id fixes the content: a different one is refused.
# Usage: sh tools/tts/fetch-audiocpp.sh   (network; the image builders run it before make)
set -eu
COMMIT=3bbbd1d28eb8a88b202907dc102595e762340b1c
URL=https://github.com/0xShug0/audio.cpp.git
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DIR="$ROOT/third_party/audiocpp"
if [ -d "$DIR/.git" ] && [ "$(git -C "$DIR" rev-parse HEAD 2>/dev/null)" = "$COMMIT" ]; then echo "audio.cpp $COMMIT already there"; exit 0; fi
rm -rf "$DIR"
mkdir -p "$DIR"
git -C "$DIR" init -q
git -C "$DIR" remote add origin "$URL"
git -C "$DIR" fetch -q --depth 1 origin "$COMMIT"
git -C "$DIR" checkout -q FETCH_HEAD
[ "$(git -C "$DIR" rev-parse HEAD)" = "$COMMIT" ] || { echo "fetch-audiocpp.sh: got another commit" >&2; exit 1; }
echo "audio.cpp $COMMIT in third_party/audiocpp"
