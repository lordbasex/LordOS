#!/bin/sh
# fetch-voices.sh — downloads what the voice packages are built from (docs/VOICE.md, phase 3), on the maintainer's
# machine, once per new set of models:
#
#   tools/voice/fetch-voices.sh <source folder>
#
#   <lang>/hd/{model.safetensors,tokenizer.model,embeddings/<voice>.safetensors}  Kyutai's 24-layer models (gated:
#       accept the conditions at https://huggingface.co/kyutai/pocket-tts and keep a read token in
#       ~/.cache/huggingface/token; LordOS's users never need one: they download LordOS's packages)
#   <lang>/standard.gguf, <lang>/embeddings/<voice>.safetensors  audio.cpp's Q8_0 conversions (audio-cpp/audio.cpp-gguf)
#
# Only sdk/voicecatalog.js's voices. Each file is checked against the SHA-256 Hugging Face publishes for it; a file
# already there and intact is kept. Then tools/voice/make-previews.sh and tools/voice/mkdeb-voice.mjs.
set -eu
OUT=${1:?usage: fetch-voices.sh <source folder>}
[ -f sdk/voicecatalog.js ] || { echo "fetch-voices.sh: run from the repository root" >&2; exit 1; }
TOKEN=$HOME/.cache/huggingface/token
[ -r "$TOKEN" ] || { echo "no Hugging Face token at $TOKEN (see the header)" >&2; exit 1; }
H=$(mktemp); chmod 600 "$H"; trap 'rm -f "$H"' EXIT
printf 'Authorization: Bearer %s\n' "$(cat "$TOKEN")" > "$H"
K=https://huggingface.co/kyutai/pocket-tts/resolve/main/languages
G=https://huggingface.co/audio-cpp/audio.cpp-gguf/resolve/main/PocketTTS-GGUF
VOICES=$(node --input-type=module -e "const c = await import('./sdk/voicecatalog.js'); console.log(c.VOICES.map((v) => v.id).join(' '))")
STD_VOICES=$(node --input-type=module -e "const c = await import('./sdk/voicecatalog.js'); console.log(c.VOICES.filter((v) => !v.hdOnly).map((v) => v.id).join(' '))")
LANGS=$(node --input-type=module -e "const c = await import('./sdk/voicecatalog.js'); console.log(c.VOICE_LANGS.map((l) => [l.id, l.hd, l.standard].join(':')).join(' '))")

# the SHA-256 Hugging Face publishes for a file (its LFS object id)
sha() { curl -sSf -H @"$H" "https://huggingface.co/api/models/$1/tree/main/$(dirname "$2")" |
        python3 -I -c "import json,sys; [print(x.get('lfs',{}).get('oid','')) for x in json.load(sys.stdin) if x['path']==sys.argv[1]]" "$2"; }
get() { # url, destination, expected sha256
    if [ -s "$2" ] && [ -n "$3" ] && echo "$3  $2" | sha256sum -c --quiet - 2>/dev/null; then return 0; fi
    mkdir -p "$(dirname "$2")"
    curl -sSfL --retry 3 -H @"$H" -o "$2.part" "$1"
    if [ -n "$3" ]; then echo "$3  $2.part" | sha256sum -c --quiet - || { echo "$1: wrong SHA-256" >&2; exit 1; }; fi
    mv "$2.part" "$2"
}
for L in $LANGS; do
    l=${L%%:*}; rest=${L#*:}; hd=${rest%%:*}; std=${rest#*:}
    for f in model.safetensors tokenizer.model; do get "$K/$hd/$f" "$OUT/$l/hd/$f" "$(sha kyutai/pocket-tts "languages/$hd/$f")"; done
    get "$G/$std/pocket-tts-$std-q8_0.gguf" "$OUT/$l/standard.gguf" "$(sha audio-cpp/audio.cpp-gguf "PocketTTS-GGUF/$std/pocket-tts-$std-q8_0.gguf")"
    for v in $VOICES; do
        get "$K/$hd/embeddings/$v.safetensors" "$OUT/$l/hd/embeddings/$v.safetensors" "$(sha kyutai/pocket-tts "languages/$hd/embeddings/$v.safetensors")"
    done
    for v in $STD_VOICES; do
        get "$G/$std/embeddings/$v.safetensors" "$OUT/$l/embeddings/$v.safetensors" "$(sha audio-cpp/audio.cpp-gguf "PocketTTS-GGUF/$std/embeddings/$v.safetensors")"
    done
    echo "$l: done"
done
