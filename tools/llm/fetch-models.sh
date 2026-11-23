#!/bin/sh
# fetch-models.sh — downloads the local language models Sentinela's packages are built from (docs/SENTINELA.md,
# "Models"), on the maintainer's machine, once per new set of models:
#
#   tools/llm/fetch-models.sh <source folder>
#
# Only sdk/llmcatalog.js's models: <source folder>/<file> for each, from Hugging Face's <source> repository. Each file is
# checked twice: against the SHA-256 Hugging Face publishes for it (its LFS object id) and against the one pinned in
# sdk/llmcatalog.js, which must be the same; a file already there and intact is kept. No token is needed (the
# repositories are public). Then tools/llm/mkdeb-llm.mjs.
set -eu
OUT=${1:?usage: fetch-models.sh <source folder>}
[ -f sdk/llmcatalog.js ] || { echo "fetch-models.sh: run from the repository root" >&2; exit 1; }
mkdir -p "$OUT"
MODELS=$(node --input-type=module -e "const c = await import('./sdk/llmcatalog.js'); console.log(c.LLM_MODELS.map((m) => [m.source, m.file, m.sha256].join('|')).join(' '))")
for M in $MODELS; do
    src=${M%%|*}; rest=${M#*|}; file=${rest%%|*}; pinned=${rest#*|}
    published=$(curl -sSf "https://huggingface.co/api/models/$src/tree/main" |
        python3 -I -c "import json,sys; [print(x.get('lfs',{}).get('oid','')) for x in json.load(sys.stdin) if x['path']==sys.argv[1]]" "$file")
    [ "$published" = "$pinned" ] || { echo "$src/$file: Hugging Face publishes $published, sdk/llmcatalog.js pins $pinned" >&2; exit 1; }
    dst="$OUT/$file"
    if [ -s "$dst" ] && echo "$pinned  $dst" | sha256sum -c --quiet - 2>/dev/null; then echo "$file: already here"; continue; fi
    curl -sSfL --retry 3 -o "$dst.part" "https://huggingface.co/$src/resolve/main/$file"
    echo "$pinned  $dst.part" | sha256sum -c --quiet - || { echo "$file: wrong SHA-256" >&2; rm -f "$dst.part"; exit 1; }
    mv "$dst.part" "$dst"
    echo "$file: done"
done
