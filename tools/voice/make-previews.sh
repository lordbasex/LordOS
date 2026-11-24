#!/bin/sh
# make-previews.sh — records the voice samples of lordos-voice-previews (docs/VOICE.md, phase 3): every voice of
# sdk/voicecatalog.js reading its language's sample sentence with the HD model, so a voice can be heard in
# Settings → AI → Voice before it is downloaded.
#
#   tools/voice/make-previews.sh <source folder>      (after fetch-voices.sh; needs server/build/lord-say and lord-tts)
#
# Writes <source folder>/previews/<lang>/<voice>.wav (16-bit mono, 24 kHz). The models are used where they are, through
# a temporary home whose voice folder is a hard-linked copy of the source folder (lord-tts's sandbox reads no
# symbolic link out of it). Nothing is played.
set -eu
SRC=${1:?usage: make-previews.sh <source folder>}
[ -f sdk/voicecatalog.js ] || { echo "make-previews.sh: run from the repository root" >&2; exit 1; }
SAY=server/build/lord-say
[ -x "$SAY" ] && [ -x server/build/lord-tts ] || { echo "build lord-say and lord-tts first (make -C server)" >&2; exit 1; }
SRC=$(cd "$SRC" && pwd)
# the temporary home next to the source folder, so its copy can be hard links (mktemp's /tmp is often another
# filesystem); and only the source's own models: say would fall back to the installed ones in /usr/share
T=$(mktemp -d "$SRC/.previews-home.XXXXXX"); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/.local/share/lordos/voice/models"
node --input-type=module -e "const c = await import('./sdk/voicecatalog.js'); for (const l of c.VOICE_LANGS) console.log(l.id + '\t' + l.sample); " > "$T/langs"
VOICES=$(node --input-type=module -e "const c = await import('./sdk/voicecatalog.js'); console.log(c.VOICES.map((v) => v.id).join(' '))")
while IFS='	' read -r l sample; do
    [ -f "$SRC/$l/hd/model.safetensors" ] || { echo "$l: no HD model in $SRC" >&2; exit 1; }
    M=$T/.local/share/lordos/voice/models/$l
    cp -al "$SRC/$l" "$M" 2>/dev/null || { rm -rf "$M"; cp -a "$SRC/$l" "$M"; }
    mkdir -p "$SRC/previews/$l"
    for v in $VOICES; do
        HOME=$T "$SAY" -l "$l" -q hd -v "$v" -o "$SRC/previews/$l/$v.wav" "$sample" 2>/dev/null || { echo "$l $v: failed" >&2; exit 1; }
        echo "$l/$v.wav"
    done
done < "$T/langs"
