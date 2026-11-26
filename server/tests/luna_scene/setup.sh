#!/bin/sh
# setup.sh HOME — the home folder of tests/app_scene_headless.sh luna_scene: the usual folders and a few files of
# different kinds, so Files' skin shows folders, pages of several kinds and a picture.
H=${1:?usage: setup.sh HOME}
mkdir -p "$H/Desktop" "$H/Documents/Letters" "$H/Downloads" "$H/Music" "$H/Pictures" "$H/Videos" || exit 1
printf 'Shopping list\n' >"$H/Documents/list.txt"
printf '# Notes\n' >"$H/Documents/notes.md"
printf 'print("hi")\n' >"$H/Documents/script.py"
printf '%%PDF-1.4\n' >"$H/Documents/manual.pdf"
printf 'PK' >"$H/Documents/photos.zip"
printf 'hello\n' >"$H/readme.txt"
# the shell's prompt names a demo account and computer, never the real ones (the screenshots may go into docs or the site)
printf "PS1='demo@demo-pc:\\\\w\\\\\$ '\nexport PS1\n" >"$H/.bashrc"
cp "$H/.bashrc" "$H/.bash_profile"
