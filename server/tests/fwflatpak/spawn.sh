#!/bin/bash
# spawn.sh — the sub-sandboxes Chromium (QtWebEngine, CEF, the Chromium browser) and zypak (Electron) ask Flatpak's
# Spawn for, served by LordOS (docs/FLATPAK.md, "Sub-sandboxes"; AGENTS.md E9): with EXPOSE_PIDS the new sandbox joins
# the app's own user and PID namespaces, which only the app's Spawn helper (src/fpspawner.c, in the app's Landlock
# domain) can open. Run it as the session user in a LordOS session, with passwordless sudo and flatpak; each app that
# isn't installed is skipped (install them with build/fpak-cli install app/<id>/x86_64/stable):
#   com.ktechpit.whatsie (QtWebEngine), org.chromium.Chromium (Chromium), io.freetubeapp.FreeTube (Electron/zypak)
#
#   make -C server build/fpspawn-cli && bash server/tests/fwflatpak/spawn.sh
#
# build/fpspawn-cli takes org.freedesktop.portal.Flatpak on a private session bus (the running lord-server keeps its
# own), in a runtime folder of its own with links to the session's display and sound sockets, launches the app with the
# real launcher and serves its Spawn calls; it is copied to LordOS's bin folder for the run (the launcher only places
# processes whose program is there) and removed at the end.
. "$(dirname "$0")/../fwlinux/lib.sh"
D=$(cd "$(dirname "$0")" && pwd)
B=$D/../../build
BIN=/usr/lib/lordos/bin
[ -x "$B/fpspawn-cli" ] || { echo "missing build/fpspawn-cli (make -C server build/fpspawn-cli)"; exit 2; }
command -v flatpak >/dev/null || { echo "skipped: flatpak is not installed"; exit 0; }
command -v dbus-daemon >/dev/null || { echo "skipped: needs dbus-daemon"; exit 0; }
has_app() { flatpak info --user "$1" >/dev/null 2>&1; }
RT=/run/user/$U/lordos-spawn-test
LOGS=/var/log/lordos/users/$(id -un)
mkdir -p "$RT/pulse"; chmod 700 "$RT"
for s in wayland-0 pipewire-0 pulse/native lord-xauth.0; do [ -e "/run/user/$U/$s" ] && [ ! -e "$RT/$s" ] && ln "/run/user/$U/$s" "$RT/$s"; done
cat > "$RT/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:path=$RT/bus</listen>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/></policy></busconfig>
EOF
dbus-daemon --config-file="$RT/bus.conf" --fork --print-pid > "$RT/dbus.pid"
sudo install -m755 "$B/fpspawn-cli" "$BIN/"
fscope() { echo "$SESS/app-lordos-fp-$1.scope"; }
procs() { cat "$(fscope "$1")/cgroup.procs" 2>/dev/null; }
stop_app() {
    local p
    for p in $(procs "$1"); do [ "$(cat /proc/$p/comm 2>/dev/null)" = lordos-view ] && kill "$p" 2>/dev/null; done
    for i in $(seq 1 20); do [ -z "$(procs "$1")" ] && return; sleep 0.3; done
}
cleanup() {
    for a in com.ktechpit.whatsie org.chromium.Chromium io.freetubeapp.FreeTube; do stop_app "$a"; done
    kill "$(cat "$RT/dbus.pid")" 2>/dev/null
    sudo rm -f "$BIN/fpspawn-cli"
    rm -rf "$RT"
}
trap cleanup EXIT
serve() {   # serve <id> <seconds>: as the session's lord-server would launch it
    local env="HOME=$HOME USER=$(id -un) LANG=C.UTF-8 PATH=/usr/bin:/bin XDG_RUNTIME_DIR=$RT WAYLAND_DISPLAY=wayland-0"
    env="$env DBUS_SESSION_BUS_ADDRESS=unix:path=$RT/bus LORDOS_SESSIOND=1 XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=LordOS"
    [ -S /tmp/.X11-unix/X0 ] && env="$env DISPLAY=:0" && [ -e "$RT/lord-xauth.0" ] && env="$env XAUTHORITY=$RT/lord-xauth.0"
    sudo sh -c "echo \$\$ > $SESS/server/cgroup.procs && exec setpriv --reuid=$U --regid=$G --init-groups env -i $env \"\$@\"" sh "$BIN/fpspawn-cli" "$1" "$2" >>"$RT/serve.log" 2>&1 &
}
# the deepest PID namespace level among the app's processes: 2 = the app's own sandbox, 3 = a sub-sandbox inside it
depth() { local p n max=0; for p in $(procs "$1"); do n=$(awk '/^NSpid/{print NF-1}' /proc/$p/status 2>/dev/null); [ "${n:-0}" -gt $max ] && max=$n; done; echo $max; }

check_app() {   # check_app <id> <engine>
    local id=$1 what=$2
    has_app "$id" || { echo "  skipped: $id ($what) is not installed"; return; }
    local log="$LOGS/apps/$id.log" comp="$LOGS/compositor.log"
    local n0=$(wc -l < "$log" 2>/dev/null || echo 0) c0=$(wc -l < "$comp" 2>/dev/null || echo 0)
    local t0=$(sudo dmesg | grep -c 'trap int3') f0=$fail
    stop_app "$id"
    serve "$id" 25
    local server=$!
    sleep 20
    local new=$(tail -n +$((n0 + 1)) "$log" 2>/dev/null)
    echo "$new" | grep -q "spawned a process of $id (sub-sandbox)" && ok "$what: its Spawn was served (sub-sandbox)" || bad "$what: no Spawn served"
    echo "$new" | grep -q "creation of new user namespaces was not disabled" && bad "$what: bubblewrap refused to join the app's namespaces" \
        || ok "$what: bubblewrap joined the app's namespaces"
    [ "$(depth "$id")" -ge 4 ] && ok "$what: helpers run in a sandbox nested in the app's PID namespace" || bad "$what: no nested sandbox ($(depth "$id") levels)"
    [ -n "$(procs "$id" | while read p; do [ "$(cat /proc/$p/comm 2>/dev/null)" = lordos-spawn ] && echo y; done)" ] \
        && ok "$what: the Spawn helper is in the app's own scope" || bad "$what: no Spawn helper in the app's scope"
    tail -n +$((c0 + 1)) "$comp" 2>/dev/null | grep -q "wayland: $id: buffer" && ok "$what: its window drew a frame" || bad "$what: no window"
    [ "$(sudo dmesg | grep -c 'trap int3')" = "$t0" ] && ok "$what: no crash (no CHECK failure)" || bad "$what: crashed (int3, a CHECK failure)"
    stop_app "$id"
    wait "$server" 2>/dev/null       # the portal's name is free again for the next app
    [ "$fail" = "$f0" ] || sed 's/^/    fpspawn-cli: /' "$RT/serve.log"
    : > "$RT/serve.log"
}

echo "1. Chromium's and zypak's sub-sandboxes (EXPOSE_PIDS), through the app's Spawn helper"
check_app com.ktechpit.whatsie "Whatsie (QtWebEngine)"
check_app org.chromium.Chromium "Chromium"
check_app io.freetubeapp.FreeTube "FreeTube (Electron)"
trap - EXIT
cleanup
fwlinux_done
