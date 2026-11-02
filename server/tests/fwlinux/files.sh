#!/bin/bash
# files.sh — "no access to your files" for Linux apps: the launcher (src/apps.c) starts a
# restricted app under Landlock (sec/sandbox.c, sandbox_apply_files). Uses build/launch-cli, the
# real launcher code outside the desktop. Applies at the next launch.
#
#   bash server/tests/fwlinux/files.sh        (from server/, after make -C server test)
#
# 1-3: a stand-in app (files_probe.py) through a .desktop: it can't read ~/Documents, the home
#      folder or firewall.json, can use its own folders, /tmp and the runtime dir, and can open
#      the one file it was opened with (not the one next to it).
# 4:   a real GTK 4 app (gtk4-print-editor, under strace) opened with a file: it starts, reads
#      that file, and is refused ~/Documents.
. "$(dirname "$0")/lib.sh"
D=$(cd "$(dirname "$0")" && pwd)
CLI=$D/../../build/launch-cli
[ -x "$CLI" ] || { echo "missing build/launch-cli (make -C server test)"; exit 2; }
APPS=~/.local/share/applications
mkdir -p "$APPS" ~/Documents ~/fwlinux-open
echo secret > ~/Documents/fwlinux-secret.txt
echo "opened with" > ~/fwlinux-open/note.txt
echo sibling > ~/fwlinux-open/fwlinux-sibling.txt
mkdir -p ~/.config/fwprobe ~/.cache/fwprobe ~/.cache/fontconfig
echo shm > /dev/shm/fwlinux-shm-$U                  # the session's shared memory: not visible to the app
PROBE=~/.config/fwprobe/probe.py           # its own config dir: visible to the restricted app
cp "$D/files_probe.py" "$PROBE"
SPID=$(pgrep -u "$U" -o -x axia-server || echo 1)   # a session process: its /proc/<pid>/root must not be reachable
TSOCK=/tmp/fwlinux-tmp-$U.sock                       # a socket in the real /tmp (like a terminal multiplexer's)
rm -f "$TSOCK"
python3 -c "import socket,sys,time; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); s.listen(); time.sleep(60)" "$TSOCK" &
TSOCK_PID=$!
cat > "$APPS/fwprobe.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=Files restriction probe
Exec=python3 $PROBE --outside-pid=$SPID --tmp-socket=$TSOCK %f
Path=/run/user/$U
MimeType=text/plain;
NoDisplay=false
DESK
OUT=~/.cache/fwprobe/result.json
probe() {      # probe [file]: launches and waits for the result
    rm -f "$OUT"
    WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/$U "$CLI" fwprobe "$@" >/dev/null 2>&1
    for i in $(seq 1 300); do [ -s "$OUT" ] && break; sleep 0.1; done
}
val() { python3 -c "import json,os;print(json.load(open(os.path.expanduser('$OUT'))).get('$1','missing'))" 2>/dev/null; }
shopt -s extglob
expect() { local key=$1 want=$2 got; got=$(val "$key"); case "$got" in $want*) ok "$key → $got";; *) bad "$key → $got (expected $want)";; esac; }

echo "1. not restricted: the app reads ~/Documents (and reaches the session bus)"
restrict desktop:fwprobe ""
probe
expect read_documents ok
expect list_home ok
expect connect_session_bus ok
rm -f ~/.cache/fwprobe/escaped ~/.config/autostart/fwlinux.desktop ~/.local/share/applications/fwlinux-evil.desktop

echo "2. no-files: only its own folders, the system, /tmp and the runtime dir"
restrict desktop:fwprobe no-files
probe
expect read_documents "@(Permission denied|No such file)*"
expect list_documents "@(Permission denied|No such file)*"
expect list_home "Permission denied"
expect read_firewall_json "@(Permission denied|No such file)*"
expect write_autostart "@(Permission denied|No such file)*"
expect read_ssh "@(Permission denied|No such file)*"
expect read_system ok
expect write_tmp ok
expect write_own_config ok
expect write_own_cache ok
expect rename_in_own_config ok
expect read_desktop_files ok
expect write_runtime_dir ok
expect connect_wayland ok
expect connect_pipewire ok
expect connect_session_bus "No such file"
expect connect_systemd_private "No such file"
expect connect_pipewire_manager "No such file"
expect outside_proc_visible "No such file"
expect outside_proc_root "No such file"
expect connect_tmp_socket "No such file"
expect cwd_relative_bus "No such file"
expect session_pts_hidden ok
expect read_session_shm "No such file"
expect write_shared_cache "@(Read-only|Permission denied)*"
expect systemd_run_user "exit"
expect gdbus_start_transient_unit "exit"
[ -e ~/.cache/fwprobe/escaped ] && bad "a process escaped through the user manager" || ok "nothing started through the user manager"

echo "3. no-files, opened with a file (\"Open with\")"
probe ~/fwlinux-open/note.txt
expect read_opened_file ok
expect write_opened_file ok
expect read_next_to_opened "@(Permission denied|No such file)*"
expect read_documents "@(Permission denied|No such file)*"
echo "3b. opened with a folder that holds protected ones (~/.config): never writable"
probe ~/.config
expect write_autostart "@(Permission denied|No such file|Read-only)*"
[ -e ~/.config/autostart/fwlinux.desktop ] && bad "an autostart entry was written through ~/.config" || ok "~/.config was not granted writable"

for r in no-mic no-camera no-usb no-bluetooth; do
    echo "3c. $r alone: its home stays, but not the session bus nor the user manager (no escape from its scope)"
    restrict desktop:fwprobe $r
    probe
    expect read_documents ok
    expect write_own_config ok
    expect connect_session_bus "No such file"
    expect connect_systemd_private "No such file"
    expect connect_pipewire_manager "No such file"
    expect cwd_relative_bus "No such file"
    expect connect_pipewire ok
    expect connect_tmp_socket "No such file"
    expect read_session_shm "No such file"
    expect systemd_run_user "exit"
    [ -e ~/.cache/fwprobe/escaped ] && bad "$r: a process escaped through the user manager" || ok "$r: nothing started through the user manager"
    # its home is its own to write (startup files too: see docs/FIREWALL.md, the residual risk)
    rm -f ~/.config/autostart/fwlinux.desktop ~/.local/share/applications/fwlinux-evil.desktop ~/.cache/fwprobe/escaped
done
echo "3d. a restricted app on a kernel whose Landlock can't scope abstract sockets: not launched"
rm -f "$OUT"
LORD_SANDBOX_MAX_ABI=5 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/$U "$CLI" fwprobe >/dev/null 2>&1 && bad "launched with Landlock ABI 5" || ok "refused with Landlock ABI 5"
sleep 1; [ -s "$OUT" ] && bad "the app ran with Landlock ABI 5" || ok "the app did not run"
restrict desktop:fwprobe no-files

echo "4. malicious .desktop files: names chosen to reach other programs' folders"
EVIL=/usr/local/lib/lordos-tests/files_probe.py          # a system path: these ids get no folders of their own
sudo install -D -m 644 "$D/files_probe.py" "$EVIL"
for id in fwevil autostart applications; do
    cat > "$APPS/$id.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=ssh
Exec=python3 $EVIL --result-in-opened %f
StartupWMClass=autostart
MimeType=text/plain;
DESK
    restrict "desktop:$id" no-files
    RES=~/fwlinux-open/result-$id.json
    : > "$RES"
    WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/$U "$CLI" "$id" "$RES" >/dev/null 2>&1
    for i in $(seq 1 300); do [ -s "$RES" ] && break; sleep 0.1; done
    for key in write_autostart read_ssh write_python3_config write_local_share_applications write_bashrc read_documents; do
        got=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1])).get(sys.argv[2],'missing'))" "$RES" "$key" 2>/dev/null)
        case "$got" in "Permission denied"*|"No such file"*|"Read-only"*) ok "$id: $key → $got";; *) bad "$id: $key → $got (expected refused)";; esac
    done
    rm -f "$APPS/$id.desktop"
done
[ -e ~/.config/autostart/fwlinux.desktop ] && bad "an autostart entry was written" || ok "no autostart entry was written"
[ -e ~/.local/share/applications/fwlinux-evil.desktop ] && bad "a .desktop was written" || ok "no .desktop file was written"
sudo rm -rf /usr/local/lib/lordos-tests
rm -rf ~/.config/fwevil ~/.cache/fwevil ~/.local/share/fwevil ~/.local/state/fwevil

if command -v gtk4-print-editor >/dev/null && command -v strace >/dev/null && [ -S "/run/user/$U/wayland-0" ]; then
    echo "5. a real GTK 4 app (gtk4-print-editor, traced with strace), restricted"
    gtk() {    # gtk <name> <Exec arguments> [file]: a .desktop for the editor, launched restricted
        TR=$HOME/.cache/$1/strace.txt                # its own cache dir: the trace survives its private /tmp
        mkdir -p "$HOME/.cache/$1"
        cat > "$APPS/$1.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=GTK editor (files test)
Exec=strace -f -qq -e trace=openat -o $TR gtk4-print-editor $2
MimeType=text/plain;
DESK
        restrict "desktop:$1" no-files
        rm -f "$TR"
        WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/$U "$CLI" "$1" ${3:+"$3"}
        sleep 4
    }
    # a: its command line names a file in ~/Documents (not passed by "Open with"): refused
    gtk fwgtkdoc "$HOME/Documents/fwlinux-secret.txt"
    PID=$(pgrep -u "$U" -n -x gtk4-print-edit)
    [ -n "$PID" ] && ok "it starts and stays up under the restriction (pid $PID)" || bad "gtk4-print-editor did not stay up"
    [ -n "$PID" ] && grep -q "^NoNewPrivs:.*1" /proc/$PID/status && ok "no_new_privs is set (Landlock)" || bad "no_new_privs not set"
    grep -E "Documents/fwlinux-secret.txt.*(EACCES|ENOENT)" "$TR" >/dev/null && ok "its open of ~/Documents/fwlinux-secret.txt is refused ($(grep -oE "(EACCES|ENOENT)" "$TR" | head -1))" || bad "~/Documents was not refused: $(grep fwlinux-secret "$TR" | head -1)"
    [ -n "$PID" ] && kill "$PID" 2>/dev/null; sleep 0.5
    # b: opened with a file ("Open with" → %f): that file is readable
    gtk fwgtkopen "%f" "$HOME/fwlinux-open/note.txt"
    PID=$(pgrep -u "$U" -n -x gtk4-print-edit)
    grep -E "fwlinux-open/note.txt.*= [0-9]+$" "$TR" >/dev/null && ok "it opens the file passed by Open with" || bad "the passed file was not opened: $(grep note.txt "$TR" | head -1)"
    [ -n "$PID" ] && kill "$PID" 2>/dev/null
    rm -f "$APPS/fwgtkdoc.desktop" "$APPS/fwgtkopen.desktop"
    rm -rf ~/.cache/fwgtkdoc ~/.cache/fwgtkopen ~/.config/fwgtkdoc ~/.config/fwgtkopen ~/.local/share/fwgtkdoc ~/.local/share/fwgtkopen ~/.local/state/fwgtkdoc ~/.local/state/fwgtkopen
fi
rm -f "$APPS/fwprobe.desktop" "$APPS/fwgtk.desktop" ~/Documents/fwlinux-secret.txt "$OUT" /dev/shm/fwlinux-shm-$U ~/.cache/fontconfig/fwlinux-probe
rm -rf ~/fwlinux-open ~/.config/fwprobe ~/.cache/fwprobe ~/.local/share/fwprobe ~/.local/state/fwprobe
fwlinux_done
