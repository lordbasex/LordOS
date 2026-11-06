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
echo bye >"$S/gone.txt"
mkdir -p "$S/Gone/Inner"; echo a >"$S/Gone/a.txt"; echo b >"$S/Gone/Inner/b.txt"
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
"$RC" serve webdav "$S" --addr "127.0.0.1:$PORT" --user dav --pass davpass >"$T/dav.log" 2>&1 &
DAV=$!
sleep 1

mkfifo "$T/in" "$T/out"
ASSETS=$(cd "$(dirname "$0")/../../assets" 2>/dev/null && pwd)
# the harness is lord-server's cloud side; started again later on the same home, as a new session would
start_harness() {
    env -i HOME="$H" XDG_RUNTIME_DIR="$R" PATH=/usr/bin:/bin LORDOS_CLOUD_RCLONE="$RC" LORDOS_ASSETS="$ASSETS" "$B/cloud-harness" <"$T/in" >"$T/out" 2>>"$T/harness.log" &
    HP=$!
    exec 3>"$T/in" 4<"$T/out"
}
start_harness
# one command, its one line (lines about opened pages are skipped)
cmd() { echo "$*" >&3; while IFS= read -r L <&4; do case $L in '{"opened"'*) continue ;; esac; break; done; printf '%s' "$L"; }
has() { case $1 in *"$2"*) return 0 ;; esac; return 1; }
rcd_pid() { pgrep -u "$(id -u)" -f "rcd --rc-addr unix://$R/lordos/cloud/rc.sock" | head -1; }

echo "== Cloud integration ($(uname -m), $("$RC" version 2>/dev/null | head -1))"
r=$(cmd providers)
check "providers: rclone started and asked (17.2 item 1)" sh -c 'case "$1" in "["*"\"kind\":"*) exit 0;; esac; exit 1' x "$r"
r2=$(cmd wait '"running":true' 20)
for k in webdav sftp s3 dropbox drive crypt nextcloud; do
    check "providers has $k" has "$r" "\"kind\":\"$k\""
done
for k in alias union local memory http cache chunker combine compress hasher archive tardigrade; do
    check "providers leaves out $k" sh -c 'case "$1" in *"\"kind\":\"$2\""*) exit 1;; esac; exit 0' x "$r" "$k"
done
check "providers: rclone's own name WebDAV" has "$r" '"label":"WebDAV"'
check "providers: rclone's own name SSH/SFTP" has "$r" '"label":"SSH/SFTP"'
check "providers: the drive variant Nextcloud is ours" has "$r" '"kind":"nextcloud","label":"Nextcloud"' 
check "providers: groups derived (dropbox oauth, s3 object, webdav server)" sh -c 'case "$1" in *"\"kind\":\"s3\",\"label\""*"\"group\":\"object\""*) exit 0;; esac; exit 1' x "$r"
check "providers: verified from assets/cloud/providers.json (webdav)" sh -c 'echo "$1" | grep -q "\"kind\":\"webdav\",[^{]*\"verified\":true"' x "$r"
check "providers: a kind not verified says so (dropbox)" sh -c 'echo "$1" | grep -q "\"kind\":\"dropbox\",[^{]*\"verified\":false"' x "$r"
r=$(cmd providers)
check "providers asked again: still the list" has "$r" '"kind":"webdav"'

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

# rclone's own questions (17.2 item 3): Jottacloud's setup asks which kind of sign-in, then for a login token
r=$(cmd add jottacloud Jotta '{}')
check "add answers rclone's question (Jottacloud: config_type)" sh -c 'case "$1" in *"\"question\":"*"\"name\":\"config_type\""*"\"value\":\"standard\""*) exit 0;; esac; exit 1' x "$r"
QS=$(printf '%s' "$r" | sed -n 's/.*"state":"\([^"]*\)".*/\1/p')
check "the question's state names the account" sh -c 'case "$1" in c*:*) exit 0;; esac; exit 1' x "$QS"
r2=$(cmd state)
check "the account shows while it is being set up" has "$r2" '"configuring":true'
r=$(cmd answer "$QS" standard)
check "answer: rclone's next question" has "$r" '"question":'
QS2=$(printf '%s' "$r" | sed -n 's/.*"state":"\([^"]*\)".*/\1/p')
r=$(cmd answer "$QS2" not-a-real-login-token)
check "a wrong answer ends with an error" has "$r" '"error":'
r=$(cmd state)
check "and the half-made account is gone" sh -c 'case "$1" in *Jotta*) exit 1;; esac; exit 0' x "$r"
r=$(cmd answer "$QS2" again)
check "answering a question that's gone is refused" has "$r" '"error":'

# Sign In Again for a server account: add with the same name is an update of that account
OLD=$(cmd secret "$ID")
r=$(cmd add webdav work "{\"url\":\"http://127.0.0.1:$PORT\",\"user\":\"dav\",\"pass\":\"davpass\"}")
check "add with an existing account's name: the same account" has "$r" "\"id\":\"$ID\""
r=$(cmd add s3 Work "{}")
check "an existing name of another kind is refused" has "$r" 'cloud.nameTaken'
for i in $(seq 40); do NEW=$(cmd secret "$ID"); [ "$NEW" != "$OLD" ] && break; sleep 0.25; done
check "new credentials saved in the keyring" test "$NEW" != "$OLD"
r=$(cmd wait '"mounted":true' 30)
check "still one account, mounted again" sh -c 'case "$1" in *"\"id\":\"c"*"\"id\":\"c"*) exit 1;; *"\"mounted\":true"*) exit 0;; esac; exit 1' x "$(cmd state)"
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
for i in $(seq 120); do [ "$(cat "$S/notes.txt")" = "changed here" ] && break; sleep 0.5; done   # a slow or busy machine: up to 60 s
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
r=$(cmd wait '"up":{"files":1' 10)
check "rclone's queue holds it" has "$r" '"ok"' 
r=$(cmd resume "$ID")
for i in $(seq 40); do [ -e "$S/paused.txt" ] && break; sleep 0.5; done
check "Resume lets it go up" test "$(cat "$S/paused.txt" 2>/dev/null)" = "made while paused"

r=$(cmd link "$M/notes.txt" 7d)
check "a link on WebDAV: unsupported" has "$r" 'unsupported'
r=$(cmd links "$ID")
check "links list empty" test "$r" = "[]"
r=$(cmd website "$ID")
check "Open Website: none for an http address" test "$r" = "null"

# Delete in the cloud (17.2 item 6): through the mount, never outside an account
r=$(cmd delete "$M/gone.txt")
check "delete a file: done" has "$r" '"ok":true'
for i in $(seq 20); do [ ! -e "$S/gone.txt" ] && break; sleep 0.5; done
check "the file is gone on the server" test ! -e "$S/gone.txt"
r=$(cmd delete "$M/Gone")
check "delete a folder with what is in it" sh -c 'case "$1" in *"\"ok\":true"*"\"deleted\":"*) exit 0;; esac; exit 1' x "$r"
for i in $(seq 20); do [ ! -e "$S/Gone" ] && break; sleep 0.5; done
check "the folder is gone on the server" test ! -e "$S/Gone"
check "no .Trash folder made in the cloud" sh -c "! ls -a '$S' | grep -q '^\.Trash'"
echo keep >"$H/outside.txt"
r=$(cmd delete "$H/outside.txt")
check "delete refuses a path outside ~/Cloud" has "$r" 'cloud.badPath'
check "and leaves it" test -e "$H/outside.txt"
r=$(cmd delete "$M")
check "delete refuses the account's own folder" has "$r" 'cloud.badPath'
r=$(cmd delete "$M/../Work/notes.txt")
check "delete refuses .." has "$r" 'cloud.badPath'
check "notes.txt still there" test -e "$S/notes.txt"

# Empty Trash and Measure (17.2 items 4 and 5)
r=$(cmd state)
check "WebDAV: no trash to empty (trashSupported false)" has "$r" '"trashSupported":false'
r=$(cmd trash "$ID")
check "Empty Trash on WebDAV: unsupported" has "$r" 'unsupported'
r=$(cmd measure "$ID")
check "Measure on WebDAV: bytes and count" sh -c 'case "$1" in *"\"bytes\":"[1-9]*"\"count\":"[1-9]*) exit 0;; esac; exit 1' x "$r"
r=$(cmd state)
check "the measure is in about.used, with measuredAt" sh -c 'case "$1" in *"\"about\":{\"used\":"[1-9]*"\"measuredAt\":"*) exit 0;; esac; exit 1' x "$r"

r=$(cmd free "$M/Projects")
r=$(cmd keep "$M/Projects")
for i in $(seq 60); do r=$(cmd status "$M/Projects/big.bin"); has "$r" '"st":"kept"' && break; sleep 0.5; done
check "Keep on This Computer brings it down: kept" has "$r" '"st":"kept"'
r=$(cmd free "$M/Projects")
check "Free Up Space leaves kept files" has "$r" '"reason":"kept"'
r=$(cmd unkeep "$M/Projects")

P1=$(rcd_pid)
exec 6<"$M/notes.txt"                       # a file open in the mount: the folder is busy when it is unmounted
r=$(cmd restart)
r=$(cmd wait '"restarts":1' 30)
r2=$(cmd wait '"mounted":true' 30)
P2=$(rcd_pid)
check "Restart: a new rclone, mounted again" sh -c "[ '$r' != '${r#*ok}' ] && [ -n '$P2' ] && [ '$P1' != '$P2' ]"
check "files read after the restart" test "$(cat "$M/notes.txt")" = "changed here"
check "a busy folder was unmounted anyway: one mount there, not two" test "$(grep -c " $M fuse.rclone " /proc/self/mounts)" = 1
exec 6<&-
exec 6<"$M/notes.txt"                       # open again across a crash: the old mount answers ENOTCONN
kill -9 "$P2"
r=$(cmd wait '"restarts":2' 30)
r2=$(cmd wait '"mounted":true' 30)
P3=$(rcd_pid)
check "a crash is restarted by itself" sh -c "[ -n '$P3' ] && [ '$P3' != '$P2' ] && [ '$r' != '${r#*ok}' ]"
check "files read after the crash" test "$(cat "$M/notes.txt")" = "changed here"
check "the dead mount was cleared: one mount there" test "$(grep -c " $M fuse.rclone " /proc/self/mounts)" = 1
exec 6<&-

# a locked keyring (as after an automatic login): the account says so while rclone runs, and connects when it opens
r=$(cmd lock)
r=$(cmd restart)
r=$(cmd wait '"state":"locked"' 30)
check "keyring locked: the account says locked" has "$r" '"ok"'
r=$(cmd state)
check "with the keyring's error, not the service's" sh -c 'case "$1" in *"\"running\":true"*"\"error\":\"cloud.keyringLocked\""*) exit 0;; esac; exit 1' x "$r"
r=$(cmd measure "$ID")
check "an action on it says the keyring is locked" has "$r" 'cloud.keyringLocked'
r=$(cmd delete "$M/notes.txt")
check "delete on it says the keyring is locked (nothing removed)" sh -c 'case "$1" in *cloud.keyringLocked*) [ -e "$2" ] && exit 0;; esac; exit 1' x "$r" "$S/notes.txt"
r=$(cmd unlock)
r=$(cmd retry all)
r=$(cmd wait '"mounted":true' 30)
check "unlocked and retried: connected without a restart" has "$r" '"ok"'
check "files read after unlocking" test "$(cat "$M/notes.txt")" = "changed here"
check "still no TCP listener" sh -c "! ss -ltnpH 2>/dev/null | grep -q 'pid=$P3,'"

exec 6<"$M/notes.txt"                       # renamed while a file is open there: the old folder is busy
r=$(cmd rename "$ID" Office)
r=$(cmd wait '"name":"Office"' 20)
r2=$(cmd wait '"mounted":true' 20)
sleep 1
check "rename: mounted at ~/Cloud/Office" grep -q " $H/Cloud/Office fuse.rclone " /proc/self/mounts
check "rename: the old folder unmounted and gone, even busy" sh -c "! grep -q ' $M fuse.rclone ' /proc/self/mounts && [ ! -e '$M' ]"
exec 6<&-
M=$H/Cloud/Office

# a new session: the account comes back from accounts.json; the same kind and name, in any case, is still that account
r=$(cmd quit)
exec 3>&- 4<&-
wait "$HP" 2>/dev/null
check "rclone ended with the first session" test -z "$(rcd_pid)"
start_harness
r=$(cmd wait '"mounted":true' 30)
check "next session: the renamed account mounted again" has "$r" '"ok"'
r=$(cmd providers)
r=$(cmd add webdav office "{\"url\":\"http://127.0.0.1:$PORT\",\"user\":\"dav\",\"pass\":\"davpass\"}")
check "after a rename and a new session: same kind, other case updates it (no nameTaken)" has "$r" "\"id\":\"$ID\""
r=$(cmd wait '"mounted":true' 30)
r=$(cmd addid "$ID" webdav AnyName "{\"url\":\"http://127.0.0.1:$PORT\",\"user\":\"dav\",\"pass\":\"davpass\"}")
check "new settings by the account's id, whatever the name" has "$r" "\"id\":\"$ID\""
r=$(cmd wait '"mounted":true' 30)
r=$(cmd add s3 OFFICE '{}')
check "another kind with that name is still refused" has "$r" 'cloud.nameTaken'
r=$(cmd addid c00000000 webdav X '{}')
check "an unknown account id is refused" has "$r" 'cloud.notFound'
r=$(cmd state)
check "still one account, still named Office" sh -c 'case "$1" in *"\"id\":\"c"*"\"id\":\"c"*) exit 1;; *"\"name\":\"Office\""*) exit 0;; esac; exit 1' x "$r"
check "files read in the new session" test "$(cat "$M/notes.txt")" = "changed here"

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
