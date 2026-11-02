#!/bin/bash
# bluetooth.sh — "no Bluetooth" for Linux apps: socket(AF_BLUETOOTH) refused by axia-netperm's
# per-cgroup LSM program (docs/FIREWALL.md, "Linux apps"). Applies immediately to new sockets.
# BlueZ over D-Bus is a separate path (see the documentation).
#
#   bash server/tests/fwlinux/bluetooth.sh
. "$(dirname "$0")/lib.sh"
[ -S /run/lordos/netperm.sock ] || { echo "axia-netperm is not running"; exit 2; }
APP=desktop:fwbt
SC=$(scope fwbt)
SOCK='
import socket, errno
try:
    s = socket.socket(31, socket.SOCK_RAW, 1); s.close(); print("ok")      # AF_BLUETOOTH, BTPROTO_HCI
except OSError as e:
    print(errno.errorcode.get(e.errno, str(e.errno)))
'
INET='
import socket
socket.socket(); socket.socket(socket.AF_UNIX); print("ok")
'
bt() { in_scope "$SC" python3 -c "$SOCK" 2>&1 | tail -1; }
expect() { local want=$1 got=$2 what=$3; case "$got" in $want*) ok "$what → $got";; *) bad "$what → $got (expected $want)";; esac; }
base=$(python3 -c "$SOCK" | tail -1)    # without restriction (ok, or EAFNOSUPPORT without the module)
echo "1. not restricted"
restrict $APP ""
expect "$base" "$(bt)" "socket(AF_BLUETOOTH)"
echo "2. no-bluetooth"
restrict $APP no-bluetooth
expect EPERM "$(bt)" "socket(AF_BLUETOOTH)"
expect ok "$(in_scope "$SC" python3 -c "$INET" 2>&1 | tail -1)" "inet and unix sockets still work"
expect "$base" "$(python3 -c "$SOCK" | tail -1)" "outside the app's scope"
echo "3. escape with systemd-run --user, also into the user's pipewire.service"
in_scope "$SC" sleep 5 &
sleep 1.5
expect EPERM "$(systemd-run --user --scope --quiet python3 -c "$SOCK" 2>&1 | tail -1)" "systemd-run --user --scope (user@ union)"
PWCG=/sys/fs/cgroup/user.slice/user-$U.slice/user@$U.service/session.slice/pipewire.service
[ -d "$PWCG" ] && expect EPERM "$(systemd-run --user --scope --quiet bash -c 'echo $$ > "$0/cgroup.procs" && exec python3 -c "$1"' "$PWCG" "$SOCK" 2>&1 | tail -1)" "an escapee that moved into pipewire.service"
wait
echo "4. lifted"
restrict $APP ""
expect "$base" "$(bt)" "socket(AF_BLUETOOTH)"
fwlinux_done
