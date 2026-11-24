#!/bin/sh
# setup.sh HOME — the scene's notes (tests/app_scene_headless.sh notes_scene), made in the scene's own home: a note with
# every kind of markdown and its attachments (a PDF, a text file, a PNG drawn in the note, a WebP), a note whose links
# try to reach out (file://, javascript:, an absolute path, .., a remote image, a link planted in its folder), an
# Arabic note, a note folder that is a link to elsewhere, and an "outside" folder those try to reach.
H=${1:?usage: setup.sh HOME}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
N="$H/Notes"
mkdir -p "$N/note-20261101-100000" "$N/note-20261101-110000" "$N/note-20261101-120000" "$H/outside/elsewhere" || exit 1
printf 'top secret\n' >"$H/outside/secret.txt"
cp "$ROOT/assets/mascot/lumo-avatar-small-64.png" "$H/outside/outside.png"
printf -- '---\ntitle: Linked\n---\nfrom outside\n' >"$H/outside/elsewhere/nota.md"
ln -s "$H/outside/elsewhere" "$N/linked-folder"
W="$N/note-20261101-120000"
cat >"$W/nota.md" <<'MD'
---
title: Welcome
color: blue
created: "2026-11-01T12:00:00.000Z"
updated: "2026-11-01T12:00:00.000Z"
---
# Welcome

Some **bold**, *italic*, ~~gone~~ and `code`.

## Lists
- [x] done
- [ ] to do
1. first
2. second

> a quote

| a | b |
|---|---|
| 1 | 2 |

![lumo](lumo.png)

[📎 report.pdf](report.pdf) [📎 notes.txt](notes.txt) [📎 photo.webp](photo.webp)
MD
cp "$ROOT/assets/mascot/lumo-avatar-small-64.png" "$W/lumo.png"
printf '%%PDF-1.4\n%%%%EOF\n' >"$W/report.pdf"
printf 'just the text file\n' >"$W/notes.txt"
printf 'RIFF\0\0\0\0WEBP' >"$W/photo.webp"
B="$N/note-20261101-110000"
cat >"$B/nota.md" <<MD
---
title: Hostile links
color: pink
created: "2026-11-01T11:00:00.000Z"
updated: "2026-11-01T11:00:00.000Z"
---
# Hostile links

[fileurl](file://$H/outside/secret.txt) [abspath]($H/outside/secret.txt) [dotdot](../../outside/secret.txt)
[jsurl](javascript:alert(1)) [planted](planted.txt) [notafile](nota.md) [web](https://example.com/x)

![remoteimg](https://example.com/a.png) ![absimg]($H/outside/outside.png) ![plantedimg](planted.png)
MD
ln -s "$H/outside/secret.txt" "$B/planted.txt"
ln -s "$H/outside/outside.png" "$B/planted.png"
A="$N/note-20261101-100000"
cat >"$A/nota.md" <<'MD'
---
title: ملاحظة
color: green
created: "2026-11-01T10:00:00.000Z"
updated: "2026-11-01T10:00:00.000Z"
---
# ملاحظة

مرحبا بالعالم، هذه ملاحظة باللغة العربية.
- [ ] مهمة
MD
