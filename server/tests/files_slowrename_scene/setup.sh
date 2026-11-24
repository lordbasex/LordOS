#!/bin/sh
# setup.sh HOME — the scene's folder (tests/app_scene_headless.sh files_slowrename_scene): Slow with nine small files.
H=${1:?usage: setup.sh HOME}
mkdir -p "$H/Slow" || exit 1
for n in alpha bravo charlie delta echo foxtrot golf hotel india; do printf '%s\n' "$n" >"$H/Slow/$n.txt"; done
