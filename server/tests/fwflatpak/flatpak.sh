#!/bin/bash
# flatpak.sh — the Flatpak test plan of docs/FLATPAK.md, automated: Flathub's firewall component, an app's identity,
# what is never granted (even with a user override), no files, no internet, the sub-sandbox LordOS serves (E9), the X
# display and the PipeWire guard with Flatpak clients. Run it in the test VM as the session user, with passwordless
# sudo, a LordOS session, lord-netperm with BPF, flatpak, and these apps installed (the steps whose app is missing
# are skipped): org.gnome.TextEditor (required), de.haeckerfelix.Shortwave and org.gnome.Decibels (network, sound).
#
#   make -C server build/launch-cli build/fpak-cli build/pwguard-cli && bash server/tests/fwflatpak/flatpak.sh
#
# The launcher only places processes whose program is in LordOS's bin folder, so build/launch-cli and build/fpak-cli
# are copied there for the run and removed at the end; firewall.json and the apps' overrides are put back.
. "$(dirname "$0")/../fwlinux/lib.sh"
D=$(cd "$(dirname "$0")" && pwd)
B=$D/../../build
BIN=/usr/lib/lordos/bin
for t in launch-cli fpak-cli pwguard-cli; do [ -x "$B/$t" ] || { echo "missing build/$t (make -C server build/$t)"; exit 2; }; done
command -v flatpak >/dev/null || { echo "skipped: flatpak is not installed"; exit 0; }
has_app() { flatpak info --user "$1" >/dev/null 2>&1; }
has_app org.gnome.TextEditor || { echo "skipped: needs org.gnome.TextEditor installed (fpak-cli install app/org.gnome.TextEditor/x86_64/stable)"; exit 0; }
export XDG_RUNTIME_DIR=/run/user/$U
TMPD=$(mktemp -d); chmod 755 "$TMPD"
sudo install -m755 "$B/launch-cli" "$B/fpak-cli" "$BIN/"
GPID=""
cleanup() {
    [ -n "$GPID" ] && kill "$GPID" 2>/dev/null
    for a in org.gnome.TextEditor de.haeckerfelix.Shortwave org.gnome.Decibels; do stop_app "$a"; flatpak override --user --reset "$a" 2>/dev/null; done
    pw-cli destroy fpk-testmic >/dev/null 2>&1
    sudo rm -f "$BIN/launch-cli" "$BIN/fpak-cli"
    rm -rf "$TMPD"
}

# in_leaf <program> [args]: as the session's lord-server would run it (its scope request goes to lord-sessiond)
in_leaf() {
    local env="HOME=$HOME USER=$(id -un) LANG=C.UTF-8 PATH=/usr/bin:/bin XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}"
    env="$env DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus LORDOS_SESSIOND=1"
    [ -S /tmp/.X11-unix/X0 ] && env="$env DISPLAY=:0" && [ -f "$XDG_RUNTIME_DIR/lord-xauth.0" ] && env="$env XAUTHORITY=$XDG_RUNTIME_DIR/lord-xauth.0"
    sudo sh -c "echo \$\$ > $SESS/server/cgroup.procs && exec setpriv --reuid=$U --regid=$G --init-groups env -i $env \"\$@\"" sh "$@"
}
fscope() { echo "$SESS/app-lordos-fp-$1.scope"; }
procs() { cat "$(fscope "$1")/cgroup.procs" 2>/dev/null; }
# the app's own newest process (not the view, bubblewrap, its proxy or a spawn waiter)
main_pid() {
    local p c last=""
    for p in $(procs "$1"); do
        c=$(cat /proc/$p/comm 2>/dev/null)
        case $c in lordos-view|bwrap|xdg-dbus-proxy|lordos-spawn|flatpak-spawn|"") ;; *) last=$p ;; esac
    done
    echo "$last"
}
view_pid() { local p; for p in $(procs "$1"); do [ "$(cat /proc/$p/comm 2>/dev/null)" = lordos-view ] && { echo $p; return; }; done; }
stop_app() { local p; for p in $(procs "$1"); do kill "$p" 2>/dev/null; done; for i in 1 2 3 4 5 6 7 8 9 10; do [ -z "$(procs "$1")" ] && return; sleep 0.3; done; }
launch() {
    stop_app "$1"
    in_leaf "$BIN/launch-cli" --flatpak "$1" >/dev/null 2>&1
    for i in $(seq 1 40); do [ -n "$(main_pid "$1")" ] && { sleep 2; return 0; }; sleep 0.5; done
    return 1
}
# in_app <id> <shell command>: as the app's own process: its namespaces, scope and environment
cat > "$TMPD/inapp.py" <<'PY'
# runs as root: never with the app's own environment (its PATH or LD_PRELOAD would pick what root executes,
# Claude Security F4). nsenter by absolute path with a fixed environment; the command inside the app's namespaces
# gets only the app's variables it needs, and a PATH of its own runtime
import os, sys
pid, scope, cmd, uid, gid = sys.argv[1:6]
KEEP = ('HOME', 'XDG_RUNTIME_DIR', 'WAYLAND_DISPLAY', 'DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'PULSE_SERVER', 'FLATPAK_ID', 'LANG')
app = dict(l.split('=', 1) for l in open(f'/proc/{pid}/environ').read().split('\0') if '=' in l)
inner = ['/usr/bin/env', '-i', 'PATH=/app/bin:/usr/bin:/bin'] + [f'{k}={app[k]}' for k in KEEP if k in app and '\n' not in app[k]]
open(f'{scope}/cgroup.procs', 'w').write(str(os.getpid()))
os.execve('/usr/bin/nsenter', ['nsenter', '-t', pid, '-a', '-r', '-w', '-S', uid, '-G', gid] + inner + ['/bin/sh', '-c', cmd],
          {'PATH': '/usr/sbin:/usr/bin:/sbin:/bin'})
PY
in_app() { sudo python3 -I "$TMPD/inapp.py" "$(main_pid "$1")" "$(fscope "$1")" "$2" "$U" "$G" 2>&1; }
since() { date '+%Y-%m-%d %H:%M:%S'; }
denied_since() { sudo journalctl -u lord-netperm --since "$1" --no-pager 2>/dev/null | grep -c "$2: denied"; }

echo "1. Flathub's downloads are the firewall component lordos:flathub"
restrict lordos:flathub no-internet
t=$(since)
if in_leaf "$BIN/fpak-cli" appstream >/dev/null 2>&1; then bad "the catalog fetch worked while lordos:flathub was blocked"; else ok "blocked: the catalog fetch failed"; fi
[ "$(denied_since "$t" lordos:flathub)" -gt 0 ] && ok "lord-netperm logged lordos:flathub's denied connections" || bad "no denial logged for lordos:flathub"
restrict lordos:flathub ""
in_leaf "$BIN/fpak-cli" appstream >/dev/null 2>&1 && ok "lifted: the catalog fetch worked" || bad "the catalog fetch failed with nothing blocked"

echo "2. Identity: every process of the app in its own scope"
restrict flatpak:org.gnome.TextEditor ""
if launch org.gnome.TextEditor; then
    ok "Text Editor started ($(procs org.gnome.TextEditor | wc -l) processes in app-lordos-fp-org.gnome.TextEditor.scope)"
    n=$(for p in $(procs org.gnome.TextEditor); do cat /proc/$p/comm; done | grep -c xdg-dbus-proxy)
    [ "$n" -ge 1 ] && ok "its D-Bus proxy runs in the same scope" || bad "no D-Bus proxy in its scope"
    esc=$(ls /sys/fs/cgroup/user.slice/user-$U.slice/user@$U.service/app.slice/ 2>/dev/null | grep -c app-flatpak)
    [ "$esc" = 0 ] && ok "nothing under user@$U.service/app.slice/app-flatpak-*" || bad "$esc app-flatpak scopes under the user manager"
else bad "Text Editor didn't start"; fi

echo "3. Never granted, even with a user override (host, session-bus, system-bus, talk to systemd1)"
flatpak override --user --filesystem=host --socket=session-bus --socket=system-bus --talk-name=org.freedesktop.systemd1 org.gnome.TextEditor
launch org.gnome.TextEditor
out=$(in_app org.gnome.TextEditor '
H=$HOME
cat $H/.config/lordos/firewall.json >/dev/null 2>&1 && echo fw-readable
echo x > $H/.local/share/applications/fpk-probe.desktop 2>/dev/null && echo apps-write-inside
echo >> $H/.bashrc 2>/dev/null && echo bashrc-writable
mkdir -p $H/.config/autostart 2>/dev/null && touch $H/.config/autostart/fpk-probe.desktop 2>/dev/null && echo autostart-writable
gdbus call --session --dest org.freedesktop.systemd1 --object-path /org/freedesktop/systemd1 --method org.freedesktop.DBus.Peer.Ping 2>&1 | grep -q ServiceUnknown && echo systemd1-unknown
gdbus call --session --dest org.freedesktop.Flatpak --object-path /org/freedesktop/Flatpak/Development --method org.freedesktop.DBus.Peer.Ping 2>&1 | grep -q ServiceUnknown && echo host-spawn-unknown
[ -e /run/dbus/system_bus_socket ] && echo system-bus
[ -e $XDG_RUNTIME_DIR/systemd/private ] && echo systemd-private
true')
echo "$out" | grep -q fw-readable && bad "firewall.json readable" || ok "firewall.json hidden"
[ -e ~/.local/share/applications/fpk-probe.desktop ] && { bad "a .desktop written inside reached the home"; rm -f ~/.local/share/applications/fpk-probe.desktop; } || ok "a .desktop written inside never reached ~/.local/share/applications"
echo "$out" | grep -q bashrc-writable && bad "~/.bashrc writable" || ok "~/.bashrc read-only"
[ -e ~/.config/autostart/fpk-probe.desktop ] && { bad "an autostart entry was created"; rm -f ~/.config/autostart/fpk-probe.desktop; } || ok "no autostart entry can be created"
echo "$out" | grep -q systemd1-unknown && ok "org.freedesktop.systemd1 unknown through the proxy" || bad "systemd1 reachable"
echo "$out" | grep -q host-spawn-unknown && ok "org.freedesktop.Flatpak (flatpak-spawn --host) unknown" || bad "org.freedesktop.Flatpak reachable"
echo "$out" | grep -q system-bus && bad "the system bus socket is visible" || ok "no system bus"
echo "$out" | grep -q systemd-private && bad "the user manager's socket is visible" || ok "no systemd user manager socket"
flatpak override --user --reset org.gnome.TextEditor

echo "4. No access to files"
restrict flatpak:org.gnome.TextEditor no-files
launch org.gnome.TextEditor
out=$(in_app org.gnome.TextEditor 'ls -A $HOME | tr "\n" " "; echo; [ -e $HOME/Documents ] && echo documents; [ -e $HOME/.ssh ] && echo ssh; [ -e /run/flatpak/bus ] && echo bus; true')
echo "$out" | grep -qE "^documents|^ssh" && bad "the home is visible ($out)" || ok "the home is a private one ($(echo "$out" | head -1))"
echo "$out" | grep -q "^bus" && bad "it has a session bus" || ok "no session bus"
restrict flatpak:org.gnome.TextEditor ""

echo "5. The sub-sandbox LordOS serves (org.freedesktop.portal.Flatpak, E9)"
launch org.gnome.TextEditor
out=$(in_app org.gnome.TextEditor 'flatpak-spawn --sandbox sh -c "exit 7"; echo status=$?')
# a longer one, left running from outside (a "&" inside sudo's pty would end with the shell's hangup)
( in_app org.gnome.TextEditor 'flatpak-spawn --sandbox sleep 8' >/dev/null 2>&1 & )
sleep 4
echo "$out" | grep -q "status=7" && ok "a sandboxed spawn ran and its exit status came back (7)" || bad "spawn: $(echo "$out" | tail -2 | tr '\n' ' ')"
sp=""
for p in $(procs org.gnome.TextEditor); do [ "$(cat /proc/$p/comm)" = sleep ] && sp=$p; done
if [ -n "$sp" ]; then
    ok "the spawned process runs in Text Editor's own scope"
    grep -q "NoNewPrivs:.*1" /proc/$sp/status && ok "no_new_privs set" || bad "no_new_privs not set"
    [ "$(sudo readlink /proc/$sp/ns/net)" != "$(sudo readlink /proc/1/ns/net)" ] && ok "its own network namespace (no network)" || bad "it shares the host's network"
    [ "$(sudo readlink /proc/$sp/ns/pid)" != "$(sudo readlink /proc/$(view_pid org.gnome.TextEditor)/ns/pid)" ] && ok "below the view's PID namespace" || bad "same PID namespace as the view"
    [ "$(awk '/^NSpid/{print NF}' /proc/$sp/status)" -ge 4 ] && ok "inside the view (NSpid $(awk '/^NSpid/{$1="";print}' /proc/$sp/status))" || bad "not nested in the view"
else bad "the spawned process wasn't found in the app's scope"; fi
# a caller that isn't a Flatpak app is refused
r=$(busctl --user call org.freedesktop.portal.Flatpak /org/freedesktop/portal/Flatpak org.freedesktop.portal.Flatpak Spawn \
    'ayaaya{uh}a{ss}ua{sv}' 2 47 0 1 5 116 114 117 101 0 0 0 0 0 2>&1)
echo "$r" | grep -qi "denied\|only a flatpak" && ok "a process outside any Flatpak app is refused" || bad "Spawn from outside: $r"

echo "6. The X display"
if [ -S /tmp/.X11-unix/X0 ]; then
    launch org.gnome.TextEditor
    v=$(view_pid org.gnome.TextEditor)
    sudo test -S /proc/$v/root/tmp/.X11-unix/X0 && ok "an unrestricted app's view has the X socket" || bad "no X socket in an unrestricted app's view"
    restrict flatpak:org.gnome.TextEditor no-usb
    launch org.gnome.TextEditor
    v=$(view_pid org.gnome.TextEditor)
    sudo test -e /proc/$v/root/tmp/.X11-unix/X0 && bad "a restricted app's view has the X socket" || ok "a restricted app's view has none"
    restrict flatpak:org.gnome.TextEditor ""
else echo "  skipped: no Xwayland socket in this session"; fi
stop_app org.gnome.TextEditor

echo "7. No internet for one app"
if has_app de.haeckerfelix.Shortwave; then
    restrict flatpak:de.haeckerfelix.Shortwave no-internet
    t=$(since)
    launch de.haeckerfelix.Shortwave
    out=$(in_app de.haeckerfelix.Shortwave 'python3 -c "import socket;s=socket.socket();s.settimeout(5);s.connect((\"1.1.1.1\",443));print(\"connected\")" 2>&1 | tail -1')
    echo "$out" | grep -q connected && bad "it connected while blocked" || ok "blocked: $(echo "$out" | cut -c1-60)"
    [ "$(denied_since "$t" flatpak:de.haeckerfelix.Shortwave)" -gt 0 ] && ok "lord-netperm logged its denied connections" || bad "no denial logged"
    restrict flatpak:de.haeckerfelix.Shortwave ""
    out=$(in_app de.haeckerfelix.Shortwave 'python3 -c "import socket;s=socket.socket();s.settimeout(5);s.connect((\"1.1.1.1\",443));print(\"connected\")" 2>&1 | tail -1')
    echo "$out" | grep -q connected && ok "lifted: it connects" || bad "lifted, but: $out"
else echo "  skipped: needs de.haeckerfelix.Shortwave"; fi

echo "8. The PipeWire guard with Flatpak apps (no microphone for one, the other unaffected)"
if has_app de.haeckerfelix.Shortwave && has_app org.gnome.Decibels && [ -S "$XDG_RUNTIME_DIR/pipewire-0-manager" ]; then
    pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=fpk-testmic node.description=fpk-testmic media.class=Audio/Source/Virtual audio.channels=2 audio.position=[FL FR] object.linger=true }" >/dev/null
    restrict flatpak:org.gnome.Decibels no-mic
    "$B/pwguard-cli" 90 > "$TMPD/guard.log" 2>&1 &
    GPID=$!
    sleep 1
    launch de.haeckerfelix.Shortwave; launch org.gnome.Decibels
    rec() { in_app "$1" 'rm -f /tmp/c.wav; timeout 3 parecord -d fpk-testmic /tmp/c.wav >/dev/null 2>&1; stat -c %s /tmp/c.wav 2>/dev/null || echo 0' | tail -1; }
    a=$(rec de.haeckerfelix.Shortwave); b=$(rec org.gnome.Decibels)
    [ "${a:-0}" -gt 1000 ] && ok "Shortwave records while Decibels runs restricted ($a bytes)" || bad "Shortwave got $a bytes"
    [ "${b:-0}" -le 1000 ] && ok "Decibels, no microphone, records nothing ($b bytes)" || bad "Decibels recorded $b bytes"
    stop_app org.gnome.Decibels; sleep 2
    a=$(rec de.haeckerfelix.Shortwave)
    [ "${a:-0}" -gt 1000 ] && ok "after Decibels closed, Shortwave still records ($a bytes)" || bad "after Decibels closed: $a bytes"
    kill "$GPID" 2>/dev/null; GPID=""
    restrict flatpak:org.gnome.Decibels ""
else echo "  skipped: needs Shortwave, Decibels and the session's PipeWire"; fi

cleanup
fwlinux_done
