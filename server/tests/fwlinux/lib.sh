# lib.sh — shared by the per-restriction end-to-end tests of Linux apps (docs/FIREWALL.md, "Linux
# apps"). Run them in the test VM as the session user, with passwordless sudo, a LordOS session
# (lord-sessiond) and axia-netperm running. Each script builds an app scope by hand, the way the
# launcher's root-owned scopes look (system.slice/lordos.service/lordos-session-<uid>/
# app-lordos-<id>.scope), writes ~/.config/axia/firewall.json and puts it back at the end.
set -u
U=$(id -u)
G=$(id -g)
SESS=/sys/fs/cgroup/system.slice/lordos.service/lordos-session-$U
FW=~/.config/axia/firewall.json
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok:   $*"; }
bad() { fail=$((fail+1)); echo "  BAD:  $*"; }
[ -d "$SESS" ] || { echo "no LordOS session for uid $U ($SESS)"; exit 2; }
mkdir -p ~/.config/axia
FW_BACKUP=$(mktemp); FW_HAD=0
[ -f "$FW" ] && { cp "$FW" "$FW_BACKUP"; FW_HAD=1; }
SCOPES=""

# scope <id>: creates app-lordos-<id>.scope (root-owned, like the real ones) and prints its path
scope() {
    local sc=$SESS/app-lordos-$1.scope
    sudo mkdir -p "$sc"
    SCOPES="$SCOPES $sc"
    echo "$sc"
}

# netperm_reload: asks axia-netperm to reread firewall.json (the only message it takes)
netperm_reload() {
    [ -S /run/lordos/netperm.sock ] || return 0
    python3 - <<'PY'
import socket, struct, time
s = socket.socket(socket.AF_UNIX); s.connect('/run/lordos/netperm.sock')
s.sendall(struct.pack('<IB', 1, 1)); time.sleep(0.4); s.close()
PY
}

# restrict <app> <flag,flag...>: only that app restricted ("" = nothing restricted)
restrict() {
    local app=$1 flags=$2 list=""
    [ -n "$flags" ] && list=$(echo "$flags" | sed 's/[^,][^,]*/"&"/g')
    if [ -n "$flags" ]; then
        echo '{"v":1,"mode":"silent","trusted":[],"restricted":{"'"$app"'":['"$list"']},"rules":[]}' > "$FW"
    else
        echo '{"v":1,"mode":"silent","trusted":[],"restricted":{},"rules":[]}' > "$FW"
    fi
    netperm_reload
}

# in_scope <scope dir> <command...>: runs the command as the user inside the scope
in_scope() {
    local sc=$1; shift
    sudo env XDG_RUNTIME_DIR=/run/user/$U DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$U/bus HOME="$HOME" \
        bash -c 'echo $$ > "$0/cgroup.procs"; exec setpriv --reuid='"$U"' --regid='"$G"' --init-groups "$@"' "$sc" "$@"
}

fwlinux_done() {
    if [ $FW_HAD = 1 ]; then cp "$FW_BACKUP" "$FW"; else rm -f "$FW"; fi
    netperm_reload
    rm -f "$FW_BACKUP"
    for sc in $SCOPES; do sudo rmdir "$sc" 2>/dev/null; done
    echo "$(basename "$0" .sh): $pass passed, $fail failed"
    [ $fail = 0 ]
}
