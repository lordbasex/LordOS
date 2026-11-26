#!/bin/bash
# lumo_e2e.sh — the `lumo` command end to end (docs/AI.md "Writing from the terminal"), by hand, from server/:
#
#   tests/lumo_e2e.sh [theme]
#
# A private session bus (dbus-run-session), a headless lord-server of this build with the theme's bundle (the shell
# tells it Writing's switch, lord.writing.serve) and a private runtime folder, so its engine is its own: build/lord-llm
# starts the installed llama-server with the installed model in its sandbox. Then the real build/lumo: status, a
# correction, a translation from stdin, Spanish messages, Writing turned off in settings.json (exit 3). Needs
# /usr/bin/llama-server and a model in /usr/share/lordos/llm; never touches the session's own bus or engine.
cd "$(dirname "$0")/.." || exit 1
THEME=${1:-default}
ROOT=$(cd .. && pwd)
[ -x build/lord-server ] && [ -x build/lumo ] && [ -x build/lord-llm ] && [ -f "$ROOT/themes/$THEME/theme.bundle.js" ] || { echo "skipped: needs build/lord-server, build/lumo, build/lord-llm and themes/$THEME/theme.bundle.js"; exit 77; }
[ -x /usr/bin/llama-server ] && ls /usr/share/lordos/llm/*.gguf >/dev/null 2>&1 || { echo "skipped: needs /usr/bin/llama-server and a model in /usr/share/lordos/llm"; exit 77; }
command -v dbus-run-session >/dev/null || { echo "skipped: needs dbus-run-session"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/var/tmp}/lumo-e2e.XXXXXX") || exit 1
chmod 700 "$D"
mkdir -p "$D/home/.config/lordos"
printf '{"schema": 9, "desktopChosen": true, "welcomeShown": true, "language": "en", "mode": "dark"}\n' >"$D/home/.config/lordos/settings.json"
export D THEME ROOT
dbus-run-session -- bash -c '
fail=0
start() {
    env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 \
        LANG=en_US.UTF-8 ./build/lord-server --headless 1280x800 --theme "$ROOT/themes/$THEME/theme.bundle.js" --timeout 600 >"$D/server.log" 2>&1 &
    P=$!
    for i in $(seq 1 120); do ./build/lumo status >/dev/null 2>&1; r=$?; [ $r != 4 ] && break; sleep 0.5; done
}
stop() { kill $P 2>/dev/null; sleep 1; kill -9 $P 2>/dev/null; wait $P 2>/dev/null; }
check() { if eval "$2"; then echo "  ok: $1"; else echo "  FAILED: $1"; fail=1; fi; }
start
out=$(LANG=en_US.UTF-8 ./build/lumo status); rc=$?
echo "$ lumo status"; echo "$out"
check "status: on, a model, local" "[ $rc = 0 ] && grep -q \"Writing: on\" <<<\"\$out\" && grep -q \"Model: qwen3\" <<<\"\$out\""
t0=$(date +%s.%N)
out=$(LANG=es_AR.UTF-8 ./build/lumo corregir "ayer fuimos al cine y la pelicula estaba muy buena pero habia mucha gente"); rc=$?
t1=$(date +%s.%N)
echo "$ lumo corregir \"ayer fuimos al cine y la pelicula estaba muy buena pero habia mucha gente\"   ($(echo "$t1 - $t0" | bc) s, engine start included)"; echo "$out"
check "corregir: accents fixed" "[ $rc = 0 ] && grep -q \"película\" <<<\"\$out\" && grep -q \"había\" <<<\"\$out\""
out=$(echo "Mañana a la tarde paso por tu casa a buscar los libros que te presté." | ./build/lumo translate en); rc=$?
echo "$ echo \"Mañana a la tarde paso por tu casa a buscar los libros que te presté.\" | lumo translate en"; echo "$out"
check "translate from stdin: English" "[ $rc = 0 ] && grep -qi \"tomorrow\" <<<\"\$out\" && grep -qi \"books\" <<<\"\$out\""
out=$(LANG=es_AR.UTF-8 ./build/lumo traducir klingon hola 2>&1); rc=$?
echo "$ lumo traducir klingon hola"; echo "$out"
check "an unknown language, in Spanish: exit 2" "[ $rc = 2 ] && grep -q \"Idioma desconocido\" <<<\"\$out\""
stop
printf "{\"schema\": 9, \"desktopChosen\": true, \"welcomeShown\": true, \"language\": \"en\", \"mode\": \"dark\", \"writing\": {\"on\": false}}\n" >"$D/home/.config/lordos/settings.json"
start
out=$(LANG=es_AR.UTF-8 ./build/lumo mejorar "hola" 2>&1); rc=$?
echo "$ lumo mejorar hola   (Writing turned off in Settings)"; echo "$out"
check "Writing off: exit 3 and where to turn it on" "[ $rc = 3 ] && grep -q \"Configuración → IA → Escritura\" <<<\"\$out\""
stop
grep -a "lumo\|Writing1" "$D/server.log" | head -3
exit $fail
'
rc=$?
rm -rf "$D"
[ $rc = 0 ] && echo ok || echo FAILED
exit $rc
