#!/bin/sh
# setup.sh HOME — the scene's folder (tests/app_scene_headless.sh files_renamelayout_scene): Slow with thirteen small files (one with a name on two lines),
# more than one row of icons.
H=${1:?usage: setup.sh HOME}
mkdir -p "$H/Slow" || exit 1
for n in alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike-with-a-much-longer-name; do printf '%s\n' "$n" >"$H/Slow/$n.txt"; done
