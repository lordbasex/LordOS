#!/bin/sh
# setup.sh HOME — the home of tests/app_scene_headless.sh luna_apps_scene: a few notes, pictures and the usual folders,
# a demo shell prompt (never the real account or computer in a screenshot).
H=${1:?usage: setup.sh HOME}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
mkdir -p "$H/Documents" "$H/Pictures/Trips" "$H/Music" "$H/Videos" "$H/Desktop" || exit 1
for n in 1 2 3; do
  d="$H/Notes/note-2026112${n}-100000"; mkdir -p "$d"
  printf -- '---\ntitle: %s\ncolor: %s\ncreated: "2026-11-2%sT10:00:00.000Z"\nupdated: "2026-11-2%sT10:00:00.000Z"\n---\n# %s\n\nA demo note.\n' \
    "$(echo 'Groceries Ideas Trip' | cut -d' ' -f$n)" "$(echo 'yellow blue green' | cut -d' ' -f$n)" "$n" "$n" "$(echo 'Groceries Ideas Trip' | cut -d' ' -f$n)" >"$d/nota.md"
done
for f in lumo-avatar-small-64.png; do cp "$ROOT/assets/mascot/$f" "$H/Pictures/lumo.png" 2>/dev/null; cp "$ROOT/assets/mascot/$f" "$H/Pictures/Trips/beach.png" 2>/dev/null; done
printf "PS1='demo@demo-pc:\\\\w\\\\\$ '\nexport PS1\n" >"$H/.bashrc"
