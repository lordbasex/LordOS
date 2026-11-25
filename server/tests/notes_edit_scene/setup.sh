#!/bin/sh
# setup.sh HOME — the scene's note (tests/app_scene_headless.sh notes_edit_scene): one long note (about 300 lines) of
# long Spanish paragraphs that wrap, a heading that wraps, tab-indented lines, a line ending in CR (a file from
# Windows), emoji and CJK, and checklists.
H=${1:?usage: setup.sh HOME}
d="$H/Notes/long"
mkdir -p "$d" || exit 1
node -e '
const L = [];
L.push("---", "title: Long", "color: yellow", "created: \"2026-11-01T10:00:00.000Z\"", "updated: \"2026-11-01T10:00:00.000Z\"", "---");
L.push("# Un título de nota bastante largo que no entra en una sola fila del editor y se tiene que partir");
for (let i = 0; i < 60; i++) {
  L.push(`Párrafo ${i}: El servidor de Git se conecta al repositorio remoto que ya existe en la máquina virtual. Funciona con SSH y también con HTTPS; la configuración está en ~/.gitconfig y en .git/config del proyecto.`);
  L.push(`\tLínea con tab ${i}\tcolumna\tfinal`);
  L.push(`    - [ ] tarea sangrada ${i} con un texto que también es largo para ver cómo se parte en dos filas del editor`);
  L.push(`Fin de línea de Windows ${i}\r`);
  L.push(`Emoji 😀🎉 y CJK 中文字符测试 ${i} — ñandú, acción, pingüino`);
}
require("fs").writeFileSync(process.argv[1], L.join("\n") + "\n");
' "$d/nota.md"
