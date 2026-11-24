#!/bin/sh
# setup.sh HOME — the scene's folders (tests/app_scene_headless.sh files_marquee_scene): Marquee with 24 small files and
# Few with 8 (the list view, with room below its rows), Many with 400 (a big folder: Files draws only what is near the view, and the rubber band scrolls through it).
H=${1:?usage: setup.sh HOME}
mkdir -p "$H/Marquee" "$H/Many" "$H/Few" || exit 1
i=1; while [ $i -le 8 ]; do printf 'note %s\n' "$i" >"$H/Few/note-$i.txt"; i=$((i + 1)); done
i=1; while [ $i -le 24 ]; do n=$(printf '%02d' $i); printf 'file %s\n' "$n" >"$H/Marquee/file-$n.txt"; i=$((i + 1)); done
i=1; while [ $i -le 400 ]; do n=$(printf '%03d' $i); : >"$H/Many/item-$n.txt"; i=$((i + 1)); done
