#!/bin/sh
# handoff_headless.sh — launching an app that already runs brings its window forward, even on a desktop not in view, and
# a copy running with no window shows a new one (docs/APPS.md, "An app that is already running"; xdgact.h
# xact_take_launch). Against a headless build/lord-server on the CPU path, in a private runtime folder and home, with
# build/handoff-client (tests/handoff_client.c), an app that runs once as Google Chrome does: a later launch hands over
# to the copy running and exits without using its activation token, and that copy asks for a token of its own (no
# keyboard, no input serial) to activate the window it already has. The 2026-11-23 case: session restore put Chrome's
# only window (its profile chooser) back on the desktop it was saved on, not the one in view, and launching Chrome
# again only marked that window as wanting attention, so nothing showed.
#
# Checks, from the theme's side (a hook appended to the bundle logs "HANDOFF ok|FAIL <what>"):
#   1. restored-hidden: session restore launches the app and its window goes to desktop 2 while desktop 1 shows;
#   2. relaunch-shows: launching it again (sdk launchApp, the launcher's and the Dock's path) activates that window:
#      desktop 2 comes into view and the window has the keyboard;
#   3. unlaunched-attention: the same handoff with no launch by the person (run from here, no LordOS token) only asks for
#      attention: the desktop in view stays;
#   4. windowless-maps: a copy that closed its last window and keeps running maps a new one when launched again, and it
#      is in front.
# Run once per theme whose bundle is built (Flow, Workspaces). Run from server/ (tests/run_suites.sh does).
# Exit 77 when something it needs is missing (npm run build; make), 1 on a failure or if lord-server died.
# HANDOFF_DEBUG=1 shows the logs.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lord-server ] && [ -x build/handoff-client ] && [ -f ../themes/default/theme.bundle.js ] || { echo "needs build/lord-server, build/handoff-client and ../themes/default/theme.bundle.js"; exit 77; }
P='' D=''
stop() { [ -n "$D" ] && XDG_RUNTIME_DIR="$D" ./build/handoff-client handoff-test --quit >/dev/null 2>&1; [ -n "$D" ] && XDG_RUNTIME_DIR="$D" ./build/handoff-client handoff-bg --quit >/dev/null 2>&1
         [ -n "$P" ] && { kill $P 2>/dev/null; sleep 0.3; kill -9 $P 2>/dev/null; }; [ -n "$D" ] && rm -rf "$D"; P='' D=''; }
trap stop EXIT
CLIENT=$(pwd)/build/handoff-client

run_theme() {
    th=$1
    THEME=../themes/$th/theme.bundle.js
    [ -f "$THEME" ] || { echo "== $th: skipped (no bundle)"; return 0; }
    echo "== $th"
    D=$(mktemp -d "${TMPDIR:-/tmp}/lho.XXXXXX") || exit 1
    chmod 700 "$D"
    mkdir -p "$D/home/.config/lordos" "$D/home/.local/share/applications"
    printf '{"schema": 9, "desktopChosen": true, "language": "en", "welcomeShown": true, "desktopCount": 2, "sessionRestore": "always",\n "sessionSaved": {"clean": true, "active": 0, "windows": [{"id": "desktop:handoff-test", "f": {"x": 60, "y": 60, "w": 400, "h": 300}, "desk": 1}]}}\n' \
        >"$D/home/.config/lordos/settings.json"
    for id in handoff-test handoff-bg; do
        extra=''
        [ $id = handoff-bg ] && extra=' --windowless'
        printf '[Desktop Entry]\nType=Application\nName=%s\nExec="%s" %s%s\n' "$id" "$CLIENT" "$id" "$extra" >"$D/home/.local/share/applications/$id.desktop"
    done
    cp "$THEME" "$D/theme.js"
    cat >>"$D/theme.js" <<'EOF'
;(() => {
  const sdk = () => globalThis.__lordos.require('@lordos/sdk');
  const say = (ok, what, detail) => console.log(`HANDOFF ${ok ? 'ok' : 'FAIL'} ${what}${ok ? '' : ': ' + detail}`);
  const desk = (w) => sdk().desktopList.get().indexOf(w.desktop);
  const shown = () => sdk().desktopList.get().indexOf(sdk().activeDesktop.get());
  const win = (app) => sdk().windows.get().find((w) => w.args?.appId === app);
  let stage = 'restore', events = [];
  const orig = globalThis.__lordWayland;
  globalThis.__lordWayland = (id, type, d) => {
    const r = orig(id, type, d);
    if (type === 'map' || type === 'unmap' || type === 'activate' || type === 'attention') events.push({ stage, type, app: d && d.appId, id });
    return r;
  };
  const later = (ms, fn) => setTimeout(() => { try { fn(); } catch (e) { say(false, stage, String(e)); console.log('HANDOFF done'); } }, ms);
  const start = Date.now();
  const restored = () => {
    const w = win('handoff-test');
    if (!w) { if (Date.now() - start < 20000) later(250, restored); else { say(false, 'restored-hidden', 'no window'); console.log('HANDOFF done'); } return; }
    later(1500, () => {
      const v = win('handoff-test');
      say(desk(v) === 1 && shown() === 0, 'restored-hidden', `window on desktop ${desk(v) + 1}, desktop ${shown() + 1} shown`);
      stage = 'relaunch';
      events = [];
      sdk().launchApp('desktop:handoff-test');
      later(3000, relaunched);
    });
  };
  const relaunched = () => {
    const v = win('handoff-test');
    const ev = events.map((e) => e.type).join(',') || 'none';
    say(!!v && shown() === desk(v) && sdk().wm.focused() === v.id && events.some((e) => e.type === 'activate'), 'relaunch-shows',
      `events ${ev}, window on desktop ${v ? desk(v) + 1 : '-'}, desktop ${shown() + 1} shown, keyboard on ${sdk().wm.focused()}`);
    // back where it was: on desktop 2, desktop 1 in view, the keyboard away from it
    sdk().wm.moveToDesktop(v.id, sdk().desktopList.get()[1]);
    sdk().switchDesktop(sdk().desktopList.get()[0]);
    stage = 'unlaunched';
    events = [];
    later(500, () => console.log('HANDOFF ready-unlaunched'));
    later(4500, unlaunched);
  };
  const unlaunched = () => {
    const v = win('handoff-test');
    const ev = events.map((e) => e.type).join(',') || 'none';
    say(events.some((e) => e.type === 'attention') && !events.some((e) => e.type === 'activate') && shown() === 0 && v && desk(v) === 1, 'unlaunched-attention',
      `events ${ev}, desktop ${shown() + 1} shown`);
    stage = 'windowless';
    events = [];
    sdk().launchApp('desktop:handoff-bg');
    later(3500, () => {
      const gone = events.some((e) => e.type === 'unmap') && !win('handoff-bg');
      if (!gone) { say(false, 'windowless-maps', `the first window didn't close by itself: ${events.map((e) => e.type).join(',')}`); console.log('HANDOFF done'); return; }
      events = [];
      sdk().launchApp('desktop:handoff-bg');
      later(3000, () => {
        const v = win('handoff-bg');
        say(!!v && events.some((e) => e.type === 'map') && sdk().wm.focused() === v.id && shown() === desk(v), 'windowless-maps',
          `events ${events.map((e) => e.type).join(',') || 'none'}, window ${v ? v.id : 'none'}, keyboard on ${sdk().wm.focused()}`);
        console.log('HANDOFF done');
      });
    });
  };
  later(500, restored);
})();
EOF
    env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS -u XDG_ACTIVATION_TOKEN \
        XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 ${HANDOFF_DEBUG:+LORD_WL_DEBUG=1} LANG=en_US.UTF-8 \
        ./build/lord-server --headless 1280x800 --theme "$D/theme.js" --timeout 90 >"$D/server.log" 2>&1 &
    P=$!
    L="$D/home/.cache/lordos/shell.log"
    i=0 handed=0
    while [ $i -lt 400 ]; do
        sleep 0.25
        i=$((i + 1))
        if [ $handed = 0 ] && grep -q 'HANDOFF ready-unlaunched' "$L" 2>/dev/null; then
            # a handoff nobody asked LordOS for: no launch, no token
            XDG_RUNTIME_DIR="$D" ./build/handoff-client handoff-test >/dev/null
            handed=1
        fi
        grep -q 'HANDOFF done' "$L" 2>/dev/null && break
        kill -0 $P 2>/dev/null || break
    done
    [ -n "$HANDOFF_DEBUG" ] && { tail -40 "$D/home/.cache/lordos/compositor.log"; cat "$D/home/.cache/lordos/apps/"*.log 2>/dev/null; }
    grep -h 'HANDOFF ok\|HANDOFF FAIL' "$L" 2>/dev/null | sed 's/.*HANDOFF /  /'
    n_ok=$(grep -c 'HANDOFF ok' "$L" 2>/dev/null)
    n_bad=$(grep -c 'HANDOFF FAIL' "$L" 2>/dev/null)
    alive=1
    kill -0 $P 2>/dev/null || { echo "  lord-server is gone"; tail -20 "$D/server.log"; alive=0; }
    stop
    [ "$alive" = 1 ] && [ "${n_ok:-0}" -eq 4 ] && [ "${n_bad:-0}" -eq 0 ]
}

R=0
for th in default workspaces; do run_theme $th || R=1; done
exit $R
