#!/bin/sh
# cloud_integration.sh — lord-server's cloud code (build/cloud-harness, the same src/cloud) against a local WebDAV server
# that rclone itself serves on loopback (docs/CLOUD.md 16). Needs fuse3 and python3; runs as a normal user in a
# throwaway home, never touches ~/Cloud.
#
#   sh tests/cloud_integration.sh <build dir> <rclone binary>
#
# Checks: rclone rcd has no TCP listener, the rc folder is 0700 and the socket only the user's, no password on any
# command line or in the environment, calls without the credentials are refused (config/dump, core/command), another
# local user can't connect (when sudo works without a password), rclone.conf holds no secret; then add an account,
# mount, read, Free Up Space while not open, refused while open, write → queue → server, Pause and Resume, Keep on This
# Computer, links unsupported on WebDAV, Restart and a crash restarted by itself, rename, remove.
set -u
B=${1:?build dir}
RC=${2:?rclone binary}
case $B in /*) ;; *) B=$(pwd)/$B ;; esac
case $RC in /*) ;; *) RC=$(pwd)/$RC ;; esac
[ -x "$B/cloud-harness" ] || { echo "no $B/cloud-harness (make build/cloud-harness)"; exit 2; }
[ -x "$RC" ] || { echo "no rclone at $RC"; exit 2; }
T=$(mktemp -d "${TMPDIR:-/tmp}/cloud-int.XXXXXX") || exit 1
H=$T/home R=$T/run S=$T/srv
mkdir -p "$H" "$R" "$S/Projects"
chmod 700 "$R"
pass=0 fail=0
ok()  { pass=$((pass + 1)); printf '  ok    %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf '  FAIL  %s%s\n' "$1" "${2:+: $2}"; }
check() { n=$1; shift; if "$@"; then ok "$n"; else bad "$n"; fi; }

cleanup() {
    exec 3>&- 2>/dev/null
    [ -n "${HP:-}" ] && kill "$HP" 2>/dev/null
    [ -n "${DAV:-}" ] && kill "$DAV" 2>/dev/null
    for m in "$H"/Cloud/*; do [ -d "$m" ] && fusermount3 -u -z "$m" 2>/dev/null; done
    pkill -f "$R/lordos/cloud/rc.sock" 2>/dev/null
    sleep 0.5
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

# the "cloud": a local WebDAV server with its own user
head -c 5000000 /dev/urandom >"$S/Projects/big.bin"
echo hello >"$S/notes.txt"
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
"$RC" serve webdav "$S" --addr "127.0.0.1:$PORT" --user dav --pass davpass >"$T/dav.log" 2>&1 &
DAV=$!
sleep 1

mkfifo "$T/in" "$T/out"
env -i HOME="$H" XDG_RUNTIME_DIR="$R" PATH=/usr/bin:/bin LORDOS_CLOUD_RCLONE="$RC" "$B/cloud-harness" <"$T/in" >"$T/out" 2>"$T/harness.log" &
HP=$!
exec 3>"$T/in" 4<"$T/out"
# one command, its one line (lines about opened pages are skipped)
cmd() { echo "$*" >&3; while IFS= read -r L <&4; do case $L in '{"opened"'*) continue ;; esac; break; done; printf '%s' "$L"; }
has() { case $1 in *"$2"*) return 0 ;; esac; return 1; }
rcd_pid() { pgrep -u "$(id -u)" -f "rcd --rc-addr unix://$R/lordos/cloud/rc.sock" | head -1; }

echo "== Cloud integration ($(uname -m), $("$RC" version 2>/dev/null | head -1))"
r=$(cmd providers)
check "providers before rclone runs: an empty list" has "$r" "[]"
r=$(cmd wait '"running":true' 20)
check "rclone rcd started for the provider list" has "$r" '"ok"'
r=$(cmd sleep 500)
r=$(cmd providers)
check "providers: WebDAV, S3, the sign-in kinds" sh -c 'case "$1" in *"\"kind\":\"webdav\""*"\"kind\":\"s3\""*) exit 0;; esac; exit 1' x "$r"
check "providers: no other operating system's names" sh -c 'case "$1" in *Microsoft*|*Windows*|*OneDrive*|*iCloud*|*Apple*) exit 1;; esac; exit 0' x "$r"

P=$(rcd_pid)
check "rcd is running" test -n "$P"
check "no TCP listener of rcd" sh -c "! ss -ltnpH 2>/dev/null | grep -q 'pid=$P,'"
check "no listener of rcd at all but its socket" sh -c "! ss -lnpH 2>/dev/null | grep 'pid=$P,' | grep -qv 'rc.sock'"
check "rc folder 0700" test "$(stat -c %a "$R/lordos/cloud")" = 700
check "rc socket for the user only" sh -c "m=\$(stat -c %a '$R/lordos/cloud/rc.sock'); [ \$((0\$m & 077)) -eq 0 ]"
check "no rc password or user on rcd's command line" sh -c "! tr '\\0' ' ' </proc/$P/cmdline | grep -Eq -- '--rc-pass|--rc-user|--rc-no-auth'"
check "rc credentials only through a memfd htpasswd" sh -c "tr '\\0' ' ' </proc/$P/cmdline | grep -q -- '--rc-htpasswd /proc/self/fd/3'"
check "no RCLONE_RC_ variable in rcd's environment" sh -c "! tr '\\0' '\\n' </proc/$P/environ | grep -q '^RCLONE_RC'"
check "in-memory configuration (--config \"\")" sh -c "tr '\\0' '|' </proc/$P/cmdline | grep -q -- '|--config||'"
NOAUTH=$(python3 - "$R/lordos/cloud/rc.sock" <<'EOF'
import socket, sys
out = []
for m in ("config/dump", "core/command", "operations/list"):
    s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
    s.sendall(("POST /%s HTTP/1.0\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}" % m).encode())
    d = b""
    while True:
        c = s.recv(4096)
        if not c: break
        d += c
    out.append(d.split(b" ")[1].decode())
print(" ".join(out))
EOF
)
check "calls without the credentials refused (config/dump, core/command, operations/list: $NOAUTH)" test "$NOAUTH" = "401 401 401"
if sudo -n true 2>/dev/null; then
    if sudo -n -u nobody python3 -c "import socket,sys; s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])" "$R/lordos/cloud/rc.sock" 2>/dev/null
    then bad "another local user can't connect"; else ok "another local user can't connect"; fi
else echo "  skip  another local user (sudo needs a password)"; fi

r=$(cmd add webdav Work "{\"url\":\"http://127.0.0.1:$PORT\",\"user\":\"dav\",\"pass\":\"davpass\",\"nosuch\":\"x\"}")
check "add refuses a field the provider doesn't have" has "$r" '"field":"nosuch"'
r=$(cmd add webdav ../Work "{\"url\":\"http://127.0.0.1:$PORT\"}")
check "add refuses a name that isn't a folder name" has "$r" 'cloud.badName'
r=$(cmd add webdav Work "{\"url\":\"http://127.0.0.1:$PORT\",\"user\":\"dav\",\"pass\":\"davpass\"}")
ID=$(printf '%s' "$r" | sed -n 's/.*"id":"\([a-z0-9]*\)".*/\1/p')
check "add an account: $ID" test -n "$ID"
r=$(cmd wait '"mounted":true' 30)
check "mounted at ~/Cloud/Work" has "$r" '"ok"'
M=$H/Cloud/Work
check "the mount is a FUSE rclone mount" sh -c "grep -q ' $M fuse.rclone ' /proc/self/mounts"
check "rclone.conf 0600" test "$(stat -c %a "$H/.config/lordos/cloud/rclone.conf")" = 600
check "rclone.conf holds no secret" sh -c "! grep -Eq '^pass|davpass|^token' '$H/.config/lordos/cloud/rclone.conf' && grep -q 'url = http://127.0.0.1:$PORT' '$H/.config/lordos/cloud/rclone.conf'"
r=$(cmd secret "$ID")
check "the password is in the keyring (stand-in), obscured by rclone" sh -c 'case "$1" in *"\"pass\":"*) case "$1" in *davpass*) exit 1;; esac; exit 0;; esac; exit 1' x "$r"
check "no password in accounts.json" sh -c "! grep -q davpass '$H/.config/lordos/cloud/accounts.json'"
check "nothing secret in any file of ~/.config/lordos/cloud" sh -c "! grep -rq davpass '$H/.config/lordos/cloud'"

check "read through the mount" test "$(cat "$M/notes.txt")" = hello
want=$(sha256sum <"$S/Projects/big.bin" | cut -c1-64)
check "a 5 MB file comes down whole" test "$(sha256sum <"$M/Projects/big.bin" | cut -c1-64)" = "$want"
sleep 12                                    # past rclone's handle hold and our margin
r=$(cmd status "$M/Projects/big.bin")
check "status: on this computer" has "$r" '"st":"here"'
r=$(cmd status "$M/notes.txt")
check "status: notes on this computer" has "$r" '"st":"here"'
r=$(cmd free "$M/Projects/big.bin")
check "Free Up Space while not open" sh -c 'case "$1" in *"\"freed\":0,"*) exit 1;; *"\"freed\":"*) exit 0;; esac; exit 1' x "$r"
check "its cached copy is gone" test ! -e "$H/.cache/lordos/cloud/vfs/$ID/Projects/big.bin"
r=$(cmd status "$M/Projects/big.bin")
check "status: in the cloud" has "$r" '"st":"cloud"'
check "read again after freeing: same bytes" test "$(sha256sum <"$M/Projects/big.bin" | cut -c1-64)" = "$want"

exec 5<"$M/Projects/big.bin"
head -c 1000 <&5 >/dev/null
r=$(cmd free "$M/Projects/big.bin")
check "Free Up Space refused while open" has "$r" '"reason":"open"'
check "its cached copy stays while open" test -e "$H/.cache/lordos/cloud/vfs/$ID/Projects/big.bin"
exec 5<&-
sleep 12
r=$(cmd free "$M/Projects/big.bin")
check "and allowed once closed" sh -c 'case "$1" in *"\"reason\":\"open\""*) exit 1;; esac; exit 0' x "$r"

echo "changed here" >"$M/notes.txt"
r=$(cmd status "$M/notes.txt")
check "status after a write: going up" has "$r" '"st":"up"'
r=$(cmd free "$M/notes.txt")
check "Free Up Space refuses a file waiting to go up" has "$r" '"reason":"pending"'
check "its cached copy stays" test -e "$H/.cache/lordos/cloud/vfs/$ID/notes.txt"
r=$(cmd wait '"up":{"files":0' 20)
for i in $(seq 30); do [ "$(cat "$S/notes.txt")" = "changed here" ] && break; sleep 0.5; done
check "the write reached the server" test "$(cat "$S/notes.txt")" = "changed here"

r=$(cmd pause "$ID")
r=$(cmd wait '"state":"paused"' 10)
check "Pause: the account says paused" has "$r" '"ok"'
echo "made while paused" >"$M/paused.txt"
sleep 11
check "Pause holds the upload back (11 s, WriteBack 5 s)" test ! -e "$S/paused.txt"
r=$(cmd status "$M/paused.txt")
check "status while paused: going up" has "$r" '"st":"up"'
r=$(cmd free "$M/paused.txt")
check "Free Up Space refuses it: waiting to go up" has "$r" '"reason":"pending"'
check "rclone's queue holds it" sh -c 'case "$1" in *"\"up\":{\"files\":1"*) exit 0;; esac; exit 1' x "$(cmd state)"
r=$(cmd resume "$ID")
for i in $(seq 40); do [ -e "$S/paused.txt" ] && break; sleep 0.5; done
check "Resume lets it go up" test "$(cat "$S/paused.txt" 2>/dev/null)" = "made while paused"

r=$(cmd link "$M/notes.txt" 7d)
check "a link on WebDAV: unsupported" has "$r" 'unsupported'
r=$(cmd links "$ID")
check "links list empty" test "$r" = "[]"
r=$(cmd website "$ID")
check "Open Website: none for an http address" test "$r" = "null"

r=$(cmd free "$M/Projects")
r=$(cmd keep "$M/Projects")
for i in $(seq 60); do r=$(cmd status "$M/Projects/big.bin"); has "$r" '"st":"kept"' && break; sleep 0.5; done
check "Keep on This Computer brings it down: kept" has "$r" '"st":"kept"'
r=$(cmd free "$M/Projects")
check "Free Up Space leaves kept files" has "$r" '"reason":"kept"'
r=$(cmd unkeep "$M/Projects")

P1=$(rcd_pid)
r=$(cmd restart)
r=$(cmd wait '"restarts":1' 30)
r2=$(cmd wait '"mounted":true' 30)
P2=$(rcd_pid)
check "Restart: a new rclone, mounted again" sh -c "[ '$r' != '${r#*ok}' ] && [ -n '$P2' ] && [ '$P1' != '$P2' ]"
check "files read after the restart" test "$(cat "$M/notes.txt")" = "changed here"
kill -9 "$P2"
r=$(cmd wait '"restarts":2' 30)
r2=$(cmd wait '"mounted":true' 30)
P3=$(rcd_pid)
check "a crash is restarted by itself" sh -c "[ -n '$P3' ] && [ '$P3' != '$P2' ] && [ '$r' != '${r#*ok}' ]"
check "files read after the crash" test "$(cat "$M/notes.txt")" = "changed here"
check "still no TCP listener" sh -c "! ss -ltnpH 2>/dev/null | grep -q 'pid=$P3,'"

r=$(cmd rename "$ID" Office)
r=$(cmd wait '"name":"Office"' 20)
r2=$(cmd wait '"mounted":true' 20)
sleep 1
check "rename: mounted at ~/Cloud/Office" sh -c "grep -q ' $H/Cloud/Office fuse.rclone ' /proc/self/mounts && [ ! -e '$M' ]"

r=$(cmd remove "$ID")
check "remove" has "$r" '"ok"'
r=$(cmd wait '"accounts":[]' 20)
check "the account is gone" has "$r" '"ok"'
sleep 1
check "unmounted, the folder removed" sh -c "! grep -q ' $H/Cloud/' /proc/self/mounts && [ ! -e '$H/Cloud/Office' ]"
check "its cache removed" sh -c "[ ! -e '$H/.cache/lordos/cloud/vfs/$ID' ] && [ ! -e '$H/.cache/lordos/cloud/vfsMeta/$ID' ]"
r=$(cmd secret "$ID")
check "its keyring item removed" test "$r" = "null"
check "nothing left in rclone.conf" sh -c "! grep -q '\\[$ID\\]' '$H/.config/lordos/cloud/rclone.conf'"
check "the server still has every file (nothing deleted in the cloud)" sh -c "[ -e '$S/Projects/big.bin' ] && [ -e '$S/notes.txt' ] && [ -e '$S/paused.txt' ]"
check "rclone's log in ~/.cache/lordos/cloud.log" test -e "$H/.cache/lordos/cloud.log"
r=$(cmd quit)
sleep 1
check "rclone ends with the session" test -z "$(rcd_pid)"

echo "== cloud integration: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
