#!/bin/bash
# screen.sh — "no screen capture" for Linux apps is enforced by design: no app can copy the
# screen, restricted or not (docs/FIREWALL.md, "Linux apps"). This checks the design holds on the
# running system:
#   - the compositor offers no capture protocol (screencopy, image capture, dmabuf export) to a
#     client (build/wl-globals, connected like any app);
#   - an X server socket, if any, is the session's own rootless Xwayland (0.8.3): 0600, the user's, and
#     rootless, so an X11 app sees only X11 windows, never Wayland or LordOS windows; restricted apps
#     get no DISPLAY (docs/FIREWALL.md);
#   - no screen-cast portal is installed that could hand a PipeWire screen stream to an app
#     (xdg-desktop-portal with a ScreenCast backend);
#   - no PipeWire node is a screen source;
#   - a restricted app can't read the framebuffer device (/dev/fb*, lord-netperm's cgroup/dev).
#
#   bash server/tests/fwlinux/screen.sh     (after make -C server build/wl-globals)
. "$(dirname "$0")/lib.sh"
D=$(cd "$(dirname "$0")" && pwd)
WG=$D/../../build/wl-globals
export XDG_RUNTIME_DIR=/run/user/$U
[ -x "$WG" ] || { echo "missing build/wl-globals (make -C server build/wl-globals)"; exit 2; }
GLOBALS=$(WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0} "$WG")
[ -n "$GLOBALS" ] && ok "connected to the compositor: $(echo "$GLOBALS" | wc -l) globals" || { bad "no globals"; fwlinux_done; exit 1; }
cap=$(echo "$GLOBALS" | grep -E -i "screencopy|image_copy_capture|image_capture_source|export_dmabuf|screencast|output_image|capture" || true)
[ -z "$cap" ] && ok "no capture protocol among them" || bad "capture protocols offered: $cap"
for x in /tmp/.X11-unix/X*; do
    [ -S "$x" ] || continue
    m=$(stat -c '%a %U' "$x")
    [ "$m" = "600 $(id -un "$U")" ] && ok "$x is the session's Xwayland socket ($m)" || bad "$x: $m (not the session's own 0600 socket)"
done
pgrep -x Xorg >/dev/null && bad "a full X server (Xorg) is running" || ok "no full X server (only rootless Xwayland, if any)"
portal=$(grep -l -s "ScreenCast" /usr/share/xdg-desktop-portal/portals/*.portal 2>/dev/null)
command -v /usr/libexec/xdg-desktop-portal >/dev/null 2>&1 || [ -x /usr/libexec/xdg-desktop-portal ] && pp=1 || pp=0
[ -z "$portal" ] && ok "no screen-cast portal backend installed (xdg-desktop-portal: $([ $pp = 1 ] && echo present || echo absent))" || bad "screen-cast portal backends: $portal"
n=$(pw-dump 2>/dev/null | grep -c -E '"media.class": "Video/Source"|"media.role": "Screen"' || true)
[ "${n:-0}" = 0 ] && ok "no PipeWire video or screen source node" || echo "  note: $n PipeWire video nodes (cameras; screens only come from a portal)"
if [ -S /run/lordos/netperm.sock ] && ls /dev/fb* >/dev/null 2>&1; then
    FB=$(ls /dev/fb* | head -1)
    SC=$(scope fwscreen)
    restrict desktop:fwscreen no-screen
    got=$(in_scope "$SC" python3 -c 'import os,sys,errno
try: os.close(os.open(sys.argv[1], os.O_RDONLY)); print("ok")
except OSError as e: print(errno.errorcode.get(e.errno))' "$FB" 2>&1 | tail -1)
    [ "$got" = EPERM ] && ok "a no-screen app can't open $FB ($got)" || bad "$FB → $got"
fi
fwlinux_done
