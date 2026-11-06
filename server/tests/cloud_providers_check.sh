#!/bin/sh
# cloud_providers_check.sh — the "Check This Account" steps (docs/CLOUD.md 19) run by lord-server's cloud code
# (build/cloud-harness) against local stand-ins of three providers, to mark them verified in assets/cloud/providers.json:
#
#   webdav  rclone serve webdav on loopback, a user and password
#   sftp    the machine's own sshd on 127.0.0.1:22, a throwaway user with a throwaway key (needs sudo without a password)
#   s3      rclone serve s3 on loopback, an access key and secret, one bucket
#
#   sh tests/cloud_providers_check.sh <build dir> <rclone binary> [webdav] [sftp] [s3]
#
# Each kind: add the account, wait for its mount, then the check: about, list the root, make a .lordos-check-<random>
# folder (inside the first bucket for object storage), write 1 MiB of random bytes, wait until it went up (going up →
# on this computer, 2 min at most) and is on the server, Free Up Space, read it back with the same hash, a link if the
# provider has them, delete the file and the folder and wait until the server has neither. Then remove the account
# (nothing in the cloud is deleted by that) and stop the stand-in. Runs as a normal user in a throwaway home.
set -u
B=${1:?build dir}
RC=${2:?rclone binary}
shift 2
KINDS=${*:-webdav sftp s3}
case $B in /*) ;; *) B=$(pwd)/$B ;; esac
case $RC in /*) ;; *) RC=$(pwd)/$RC ;; esac
[ -x "$B/cloud-harness" ] || { echo "no $B/cloud-harness (make build/cloud-harness)"; exit 2; }
T=$(mktemp -d "${TMPDIR:-/tmp}/cloud-prov.XXXXXX") || exit 1
pass=0 fail=0 SUSER=''
ok()  { pass=$((pass + 1)); printf '  ok    %-44s %s\n' "$1" "${2:-}"; }
bad() { fail=$((fail + 1)); printf '  FAIL  %-44s %s\n' "$1" "${2:-}"; }
ms() { date +%s%3N; }
port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
has() { case $1 in *"$2"*) return 0 ;; esac; return 1; }

cleanup() {
    exec 3>&- 2>/dev/null
    [ -n "${HP:-}" ] && kill "$HP" 2>/dev/null
    [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null
    for m in "$T"/home/Cloud/*; do [ -d "$m" ] && fusermount3 -u -z "$m" 2>/dev/null; done
    pkill -f "$T/run/lordos/cloud/rc.sock" 2>/dev/null
    [ -n "$SUSER" ] && { sudo -n pkill -u "$SUSER"; sleep 1; sudo -n userdel -r "$SUSER" 2>/dev/null; }
    sleep 0.5
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

H=$T/home R=$T/run
mkdir -p "$H" "$R" && chmod 700 "$R"
mkfifo "$T/in" "$T/out"
env -i HOME="$H" XDG_RUNTIME_DIR="$R" PATH=/usr/bin:/bin LORDOS_CLOUD_RCLONE="$RC" "$B/cloud-harness" <"$T/in" >"$T/out" 2>"$T/harness.log" &
HP=$!
exec 3>"$T/in" 4<"$T/out"
cmd() { printf "%s\n" "$*" >&3; while IFS= read -r L <&4; do case $L in '{"opened"'*) continue ;; esac; break; done; printf '%s' "$L"; }
cmd providers >/dev/null; cmd wait '"running":true' 20 >/dev/null; cmd sleep 500 >/dev/null; cmd providers >/dev/null

echo "== Cloud providers check ($(uname -m), $("$RC" version 2>/dev/null | head -1))"

# one step: NAME, then a command; prints its time
step() { n=$1; shift; t0=$(ms); if "$@"; then ok "$n" "$(( $(ms) - t0 )) ms"; else bad "$n" "$(( $(ms) - t0 )) ms"; fi; }
waitfor() { s=$1; shift; i=0; while [ $i -lt $((s * 4)) ]; do "$@" && return 0; sleep 0.25; i=$((i + 1)); done; return 1; }

check_account() {         # KIND NAME FIELDS SERVERDIR (where the cloud keeps the files, to see them arrive and go)
    kind=$1 name=$2 fields=$3 srv=$4
    r=$(cmd add "$kind" "$name" "$fields")
    ID=$(printf '%s' "$r" | sed -n 's/.*"id":"\([a-z0-9]*\)".*/\1/p')
    if [ -z "$ID" ]; then bad "$kind: add" "$r"; return; fi
    ok "$kind: add"
    r=$(cmd wait '"mounted":true' 40)
    if ! has "$r" '"ok"'; then bad "$kind: mounted" "$(cmd state | head -c 400)"; return; fi
    M=$H/Cloud/$name
    st=$(cmd about "$ID" >/dev/null; sleep 1; cmd state)
    if has "$st" '"about":null'; then ok "$kind: about" "skipped (the provider tells no space)"
    else step "$kind: about" sh -c 'case "$1" in *"\"about\":{"*"\"used\":"*) exit 0;; esac; exit 1' x "$st"; fi
    step "$kind: list the root" ls "$M"
    base=$M sbase=$srv
    if [ "$kind" = s3 ]; then b=$(ls "$M" | head -1); base=$M/$b sbase=$srv/$b; fi
    rnd=$(head -c 6 /dev/urandom | od -An -tx1 | tr -d ' \n')
    D=$base/.lordos-check-$rnd SD=$sbase/.lordos-check-$rnd
    step "$kind: make the check folder" mkdir "$D"
    head -c 1048576 /dev/urandom >"$T/check.bin"
    want=$(sha256sum <"$T/check.bin" | cut -c1-64)
    step "$kind: write 1 MiB" cp "$T/check.bin" "$D/check.bin"
    up() { r=$(cmd status "$D/check.bin"); has "$r" '"st":"here"' || has "$r" '"st":"kept"'; }
    step "$kind: went up (going up → on this computer)" waitfor 120 up
    step "$kind: on the server, same bytes" sh -c "[ \"\$(sha256sum <'$SD/check.bin' | cut -c1-64)\" = '$want' ]"
    sleep 12
    r=$(cmd free "$D/check.bin")
    step "$kind: Free Up Space" sh -c 'case "$1" in *"\"freed\":0,"*|*"\"skipped\":[{"*) exit 1;; *"\"freed\":"*) exit 0;; esac; exit 1' x "$r"
    step "$kind: read back, same hash" sh -c "[ \"\$(sha256sum <'$D/check.bin' | cut -c1-64)\" = '$want' ]"
    if has "$(cmd state)" '"linksSupported":true'; then
        r=$(cmd link "$D/check.bin" 1d)
        if has "$r" '"url"'; then ok "$kind: make a link"; cmd unlink "$D/check.bin" >/dev/null
        # rclone serve s3 answers with an http:// link, and lord-server keeps only https links (cloud_url_ok)
        elif [ "$kind" = s3 ] && has "$r" 'cloud.linkFailed'; then ok "$kind: links" "skipped (the stand-in's links are http://, LordOS keeps https only)"
        else bad "$kind: make a link" "$r"; grep -i link "$H/.cache/lordos/cloud.log" 2>/dev/null | tail -2 | cut -c1-300; fi
    else ok "$kind: links" "skipped (the provider has none)"; fi
    step "$kind: delete the file and the folder" sh -c "rm '$D/check.bin' && rmdir '$D'"
    gone() { [ ! -e "$SD" ]; }
    step "$kind: the delete reached the server" waitfor 60 gone
    r=$(cmd remove "$ID" discard)
    has "$r" '"ok"' && ok "$kind: remove the account" || bad "$kind: remove the account" "$r"
    cmd wait '"accounts":[]' 20 >/dev/null
}

for kind in $KINDS; do
    S=$T/srv-$kind
    mkdir -p "$S"
    case $kind in
    webdav)
        P=$(port)
        "$RC" serve webdav "$S" --addr "127.0.0.1:$P" --user dav --pass davpass >"$T/srv.log" 2>&1 &
        SRV=$!; sleep 1
        check_account webdav CheckWebDAV "{\"url\":\"http://127.0.0.1:$P\",\"user\":\"dav\",\"pass\":\"davpass\"}" "$S"
        kill "$SRV"; SRV='' ;;
    sftp)
        if ! sudo -n true 2>/dev/null; then echo "  skip  sftp (needs sudo without a password for a throwaway user)"; continue; fi
        SUSER=lordchk$(head -c 3 /dev/urandom | od -An -tx1 | tr -d ' \n')
        sudo -n useradd -m -s /bin/sh "$SUSER" || { bad "sftp: throwaway user"; continue; }
        ssh-keygen -q -t ed25519 -N '' -f "$T/key" -C cloudcheck
        SH=$(getent passwd "$SUSER" | cut -d: -f6)
        sudo -n install -d -m 700 -o "$SUSER" "$SH/.ssh" && sudo -n install -m 600 -o "$SUSER" "$T/key.pub" "$SH/.ssh/authorized_keys"
        sudo -n chmod 755 "$SH"      # so this user can see the files arrive and go
        # rclone reads the key from the field, never from a file another user could read
        # (rclone's key_pem is one line, its line ends written as \n: a backslash and an n, so \\n in JSON)
        PEM=$(awk 'BEGIN{ORS="\\\\n"} {print}' "$T/key")
        check_account sftp CheckSFTP "{\"host\":\"127.0.0.1\",\"user\":\"$SUSER\",\"key_pem\":\"$PEM\"}" "$SH"
        sudo -n pkill -u "$SUSER"; sleep 1      # its sftp-server outlives rclone's connection for a moment
        sudo -n userdel -r "$SUSER" 2>/dev/null
        getent passwd "$SUSER" >/dev/null && bad "sftp: throwaway user removed" || ok "sftp: throwaway user removed"
        SUSER='' ;;
    s3)
        P=$(port)
        mkdir -p "$S/bucket"
        "$RC" serve s3 "$S" --addr "127.0.0.1:$P" --auth-key checkkey,checksecret >"$T/srv.log" 2>&1 &
        SRV=$!; sleep 1
        check_account s3 CheckS3 "{\"provider\":\"Rclone\",\"access_key_id\":\"checkkey\",\"secret_access_key\":\"checksecret\",\"endpoint\":\"http://127.0.0.1:$P\"}" "$S"
        kill "$SRV"; SRV='' ;;
    *) echo "  skip  $kind (no local stand-in)" ;;
    esac
done
cmd quit >/dev/null
echo "== cloud providers check: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
