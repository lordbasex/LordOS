#!/bin/sh
# x11-proofs.sh — the X11 screenshots in docs/wayland-proofs/: real X apps (xterm, xprop, xwininfo, xdpyinfo, zenity
# with GDK_BACKEND=x11) against a private headless lord-server's Xwayland (CPU path, its own runtime folder and home,
# never the session's); the theme saves a screenshot (960x600) each time a window titles itself "proof:<name>".
# Usage: tools/wl-conformance/x11-proofs.sh <repository> <output folder>. Needs the theme bundle and lord-server built.
R=$1 OUT=$2
SV=$R/server
THEME=$R/themes/default/theme.bundle.js
D=$(mktemp -d /tmp/lwproof.XXXXXX) || exit 1
chmod 700 "$D"
mkdir -p "$D/home/.config/lordos" "$D/home/proofs" "$OUT"
printf '{"schema": 9, "desktopChosen": true, "language": "en"}\n' >"$D/home/.config/lordos/settings.json"
printf '{ "version": 1, "apps": { "screenshots": { "screen": { "state": "allow", "when": 1 } } } }\n' >"$D/home/.config/lordos/permisos.json"
{ cat "$THEME"; sed "s|@PROOFS@|$D/home/proofs|" <<'EOF'
;(() => {
  const orig = globalThis.__lordWayland;
  const shot = (name) => {
    try {
      const sid = lord.media.screenshot('screenshots', {});
      const small = lord.media.resize(sid, 960, 600) || sid;
      lord.media.saveImage(small, '@PROOFS@/' + name + '.png', { format: 'png' });
      console.log('WLPROOF shot ' + name);
    } catch (e) { console.log('WLPROOF shot-failed ' + name + ' ' + e); }
  };
  globalThis.__lordWayland = (id, type, d) => {
    const r = orig(id, type, d);
    try {
      if (['map', 'title', 'attention', 'activate', 'parent', 'icon', 'taskbar', 'pager', 'layer', 'unmap'].includes(type)) {
        let j = ''; try { j = JSON.stringify(d); } catch (e) { j = '?'; }
        console.log('WLPROOF ' + JSON.stringify({ id, type, d: j.slice(0, 400) }));
      }
      const m = /^proof:([a-z0-9_.-]+)$/.exec((d && d.title) || '');
      if (m && (type === 'title' || type === 'map')) setTimeout(() => shot(m[1]), 1500);
    } catch (e) {}
    return r;
  };
})();
EOF
} >"$D/theme.js"
cd "$SV" && env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 LORD_HEADLESS_DRAW=1 LORD_XWM_DEBUG=1 \
    setsid "$SV/build/lord-server" --headless 1280x800 --theme "$D/theme.js" --timeout 400 --user "$(id -un)" >"$D/server.log" 2>&1 &
P=$!
cleanup() { kill -TERM -$P 2>/dev/null; sleep 1; kill -KILL -$P 2>/dev/null; pkill -KILL -f "$D" 2>/dev/null; [ -n "${KEEP:-}" ] || rm -rf "$D"; }
trap cleanup EXIT
i=0
while [ $i -lt 120 ] && ! ls "$D" | grep -q '^wayland-[0-9]*$'; do sleep 0.25; i=$((i + 1)); done
sleep 3
LOGS="$D/server.log $D/home/.cache/lordos/shell.log $D/home/.cache/lordos/compositor.log"
DISP=$(cat $LOGS 2>/dev/null | grep -o 'xwayland: DISPLAY=:[0-9]*' | head -1 | cut -d= -f2)
[ -n "$DISP" ] || { echo "no X display"; ls $D; tail -20 $D/server.log; exit 1; }
export XDG_RUNTIME_DIR="$D" HOME="$D/home" DISPLAY="$DISP" XAUTHORITY="$D/lord-xauth.${DISP#:}" LANG=C.UTF-8 LC_ALL=C.UTF-8
unset WAYLAND_DISPLAY
echo "DISPLAY=$DISPLAY"
XT="xterm -fa Monospace -fs 10 -xrm *allowWindowOps:true -xrm *bellIsUrgent:true"
title() { printf '\033]2;%s\007' "$1"; }
# a scene: an xterm running a shell script ($2), titled proof:<name> when it is ready
scene() {
    name=$1; shift
    cat >"$D/scene-$name.sh" <<EOS
#!/bin/bash
title() { printf '\\033]2;%s\\007' "\$1"; }
LOG="$D/home/.cache/lordos/compositor.log"
$*
title "proof:$name"
sleep 6
EOS
    chmod +x "$D/scene-$name.sh"
    $XT -geometry 110x34 -T "scene $name" -e "$D/scene-$name.sh" &
    wait $!
}
cat >"$D/dialog.sh" <<'EOD'
# the dialog: its WM_TRANSIENT_FOR set to the window $1 (a real xprop on a real xterm), then what X and the theme say
xprop -id "$WINDOWID" -f WM_TRANSIENT_FOR 32x -set WM_TRANSIENT_FOR "$1"
sleep 1
echo "this xterm: $WINDOWID"
xprop -id "$WINDOWID" WM_TRANSIENT_FOR
grep -h 'WLPROOF.*"parent"' "$2" | tail -1 | cut -c26-100
sleep 30
EOD
sleep 1
# 1. xwayland-shell-v1: which wl_surface shows each X window, by serial
scene startup 'xdpyinfo | head -4; echo; echo "--- the window manager (LORD_XWM_DEBUG):"; grep -h "is surface" $LOG '"$D"'/server.log | tail -6; sleep 1'
# 2. root properties: _NET_SUPPORTED, _NET_WORKAREA, the client lists
scene ewmh-root 'xprop -root _NET_SUPPORTED _NET_WORKAREA _NET_CLIENT_LIST _NET_CLIENT_LIST_STACKING _NET_ACTIVE_WINDOW | fold -w 108'
# 3. urgency: an xterm in the background rings its bell (bellIsUrgent) and asks for attention
$XT -class WlconfBell -T "rings its bell" -e sh -c 'sleep 4; printf "\a"; sleep 40' &
BELL=$!
sleep 1.5
scene urgency 'sleep 4; B=$(xwininfo -root -tree | grep "rings its bell" | head -1 | awk "{print \$1}"); echo "the background xterm: $B"; xprop -id $B WM_HINTS _NET_WM_STATE; echo; grep -h "WLPROOF.*attention" '"$D"'/home/.cache/lordos/shell.log '"$D"'/server.log | tail -2 | cut -c1-108'
kill $BELL 2>/dev/null
# 4. WM_NORMAL_HINTS: this xterm sets a 560x380 .. 760x520 range, then asks to be 10x3 characters and 200x80 characters
scene normal-hints 'xwininfo -id $WINDOWID | grep -E "Width|Height" | sed "s/^/as opened: /"; xprop -id $WINDOWID -f WM_NORMAL_HINTS 32iiiiiiiii -set WM_NORMAL_HINTS 48,0,0,0,0,560,380,760,520; sleep 1; xprop -id $WINDOWID WM_NORMAL_HINTS; xwininfo -id $WINDOWID | grep -E "Width|Height" | sed "s/^/after xprop set min 560x380, max 760x520: /"; printf "\033[8;3;10t"; sleep 1; xwininfo -id $WINDOWID | grep -E "Width|Height" | sed "s/^/after the app asked for 10x3 characters: /"'
# 5. WM_TRANSIENT_FOR: a second xterm made a dialog of this one
scene transient 'P=$WINDOWID; echo "the owner: $P"; xterm -fa Monospace -fs 10 -T "the dialog" -geometry 60x6 -e sh '"$D"'/dialog.sh $P '"$D"'/home/.cache/lordos/shell.log & sleep 3'
# 6. _NET_WM_ICON: zenity (GTK, X11 backend; no .desktop entry) sets its own icon; the Dock shows it
GDK_BACKEND=x11 zenity --info --text "An X11 GTK dialog: its own _NET_WM_ICON is in the Dock" &
ICON=$!
sleep 3
scene wm-icon 'I=$(xwininfo -root -tree | grep "\"zenity\" \"zenity\"" | grep -v "1x1" | head -1 | awk "{print \$1}"); echo "zenity: $I"; xprop -id $I WM_CLASS; xprop -id $I _NET_WM_ICON | head -3 | cut -c1-100; grep -h "WLPROOF.*\"icon\"" '"$D"'/home/.cache/lordos/shell.log | tail -2 | cut -c1-108'
kill $ICON 2>/dev/null
# 7. per-window properties the WM keeps: _NET_FRAME_EXTENTS, _NET_WM_STATE, WM_STATE, _NET_WM_PID
scene window-props 'xprop -id $WINDOWID _NET_FRAME_EXTENTS _NET_WM_STATE WM_STATE _NET_WM_PID _NET_WM_WINDOW_TYPE; echo; xprop -root _NET_WORKAREA'
# 8. always on top: this xterm asks for _NET_WM_STATE_ABOVE (the EWMH request wmctrl -b add,above sends; xprop can't
#    send one, and wmctrl isn't installed: build/x11-conformance-client netstate), then a normal xterm opens after it
#    and takes the keyboard; this one stays drawn over it, and xprop shows the state, the active window and the stacking
scene always-on-top 'C='"$SV"'/build/x11-conformance-client; sleep 1; $C netstate $WINDOWID add _NET_WM_STATE_ABOVE; sleep 1; xterm -fa Monospace -fs 10 -geometry 120x40 -T "a normal xterm opened after it: it has the keyboard" -e sh -c "sleep 40" & sleep 3; echo "this xterm: $WINDOWID, always on top"; xprop -id $WINDOWID _NET_WM_STATE; xprop -root _NET_ACTIVE_WINDOW _NET_CLIENT_LIST_STACKING; xwininfo -root -tree | grep -E "normal xterm|scene always" | cut -c1-100; grep -h "WLPROOF.*\"layer\"" '"$D"'/home/.cache/lordos/shell.log | tail -1 | cut -c1-100'
sleep 2
cp "$D"/home/proofs/*.png "$OUT"/ 2>/dev/null
ls -la "$OUT"
grep -h "WLPROOF" "$D/home/.cache/lordos/shell.log" "$D/server.log" 2>/dev/null | grep -v '"map"\|"title"\|"unmap"' | cut -c1-200 | tail -20
grep -h "is surface" $LOGS 2>/dev/null | tail -3
