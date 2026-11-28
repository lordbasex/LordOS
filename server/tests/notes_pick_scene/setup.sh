#!/bin/sh
# setup.sh HOME — the scene's notes (tests/app_scene_headless.sh notes_pick_scene): a checklist note in each paper colour,
# two of them on the desktop, an Arabic checklist, and a note whose saved place is off every display.
H=${1:?usage: setup.sh HOME}
N="$H/Notes"
mkdir -p "$N" || exit 1
i=0
for c in yellow pink green blue purple gray; do
    i=$((i + 1))
    d="$N/check-$c"
    mkdir -p "$d"
    desk=false; [ "$c" = yellow ] || [ "$c" = blue ] && desk=true
    x=$((40 + (i - 1) * 200))
    cat >"$d/nota.md" <<MD
---
title: "List $c"
color: $c
x: $x
y: 420
w: 190
h: 200
desktop: $desk
created: "2026-11-0${i}T10:00:00.000Z"
updated: "2026-11-0${i}T10:00:00.000Z"
---
# List $c
- [x] Done item stays readable
- [ ] Open item
- [ ] A longer open item that wraps onto a second line
MD
done
mkdir -p "$N/arabic"
cat >"$N/arabic/nota.md" <<'MD'
---
title: قائمة
color: green
created: "2026-11-10T10:00:00.000Z"
updated: "2026-11-10T10:00:00.000Z"
---
# قائمة المهام
- [x] شراء الخبز
- [ ] الاتصال بالطبيب
MD
mkdir -p "$N/far-away"
cat >"$N/far-away/nota.md" <<'MD'
---
title: Far away
color: purple
x: 9000
y: 5000
created: "2026-10-01T10:00:00.000Z"
updated: "2026-10-01T10:00:00.000Z"
---
# Far away
saved on a monitor that is gone
MD
