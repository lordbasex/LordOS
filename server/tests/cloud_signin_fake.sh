#!/bin/sh
# cloud_signin_fake.sh — a browser sign-in through lord-server's cloud code (build/cloud-harness, the same src/cloud)
# against tests/cloud_fake_rclone.py, a stand-in for rclone that answers OneDrive's config state machine as rclone
# v1.75.2 runs it (issue #87). No network, no FUSE, no real account: it checks what LordOS does with rclone's answers.
#
#   sh tests/cloud_signin_fake.sh <build dir>
#
# 1. one drive: the account is made (the second sign-in of the same kind too, under another name);
# 2. a stale drive listed first (driveid_final fails and sends rclone back to choose_type): LordOS goes back and takes
#    the next drive, the account is made with it;
# 3. no drive at all: the sign-in fails with cloud.signinNoDrive and rclone's words, and the half-made remote is deleted;
# 4. a password account's session (iCloud's cookies) changed in rclone survives a restart of rclone.
# 5. a saved session refused once: dropped, the trust token kept;
# 6. an account turned off and on keeps its settings and sign-in; busy, it asks, waits or is forced;
# 7. a password provider that asks for a new code at the start of the session turns itself off (rclone 9966, 9658).
set -u
B=${1:?build dir}
case $B in /*) ;; *) B=$(pwd)/$B ;; esac
[ -x "$B/cloud-harness" ] || { echo "no $B/cloud-harness (make build/cloud-harness)"; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "skipped: needs python3"; exit 0; }
FAKE=$(cd "$(dirname "$0")" && pwd)/cloud_fake_rclone.py
T=$(mktemp -d "${TMPDIR:-/tmp}/cloud-signin.XXXXXX") || exit 1
pass=0 fail=0
ok()  { pass=$((pass + 1)); printf '  ok    %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf '  FAIL  %s%s\n' "$1" "${2:+: $2}"; }
check() { n=$1; shift; if "$@"; then ok "$n"; else bad "$n" "$(printf '%s' "${r:-}" | cut -c1-400)"; fi; }
has() { case $1 in *"$2"*) return 0 ;; esac; return 1; }
hasnt() { case $1 in *"$2"*) return 1 ;; esac; return 0; }

cleanup() {
    exec 3>&- 2>/dev/null
    [ -n "${KEEP:-}" ] && cp -r "$T" "$KEEP" 2>/dev/null
    [ -n "${HP:-}" ] && kill "$HP" 2>/dev/null
    pkill -f "$T/run" 2>/dev/null
    sleep 0.3
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

ASSETS=$(cd "$(dirname "$0")/../../assets" 2>/dev/null && pwd)
# one session of the harness, the stand-in told its drives in the home (rclone gets no other environment): its home,
# its log of rc calls
run() {
    drives=$1
    H=$T/home-$drives R=$T/run/$drives
    rm -rf "$H" "$R"; mkdir -p "$H" "$R"; chmod 700 "$R"
    : >"$T/rc-$drives.log"
    printf '{"drives":"%s","log":"%s"}\n' "$drives" "$T/rc-$drives.log" >"$H/.fake-rclone.json"
    start
}
# the harness again on the same home: a new session (a logout, a reboot)
start() {
    rm -f "$T/in" "$T/out"; mkfifo "$T/in" "$T/out"
    env -i HOME="$H" XDG_RUNTIME_DIR="$R" PATH=/usr/bin:/bin LORDOS_CLOUD_RCLONE="$FAKE" LORDOS_CLOUD_FAKE_AUTHORIZE=1 \
        LORDOS_ASSETS="$ASSETS" "$B/cloud-harness" <"$T/in" >"$T/out" 2>>"$T/harness-$drives.log" &
    HP=$!
    exec 3>"$T/in" 4<"$T/out"
}
stop() { cmd quit >/dev/null; exec 3>&- 4<&-; wait "$HP" 2>/dev/null; HP=; pkill -f "$T/run" 2>/dev/null; sleep 0.2; }
cmd() { echo "$*" >&3; while IFS= read -r L <&4; do case $L in '{"opened"'*) continue ;; esac; break; done; printf '%s' "$L"; }

echo "== Cloud sign-in against a stand-in rclone (#87)"
chmod +x "$FAKE"

run real
r=$(cmd signin onedrive OneDrive)
check "one drive: the sign-in starts with the browser address" has "$r" '"url":"http://127.0.0.1:53682/auth?state=fakeState123456"'
cmd wait '"phase":"done"' 20 >/dev/null; r=$(cmd state)
check "one drive: the sign-in finishes" has "$r" '"phase":"done"'
check "one drive: the account is there" has "$r" '"name":"OneDrive","kind"'
log=$(cat "$T/rc-real.log")
check "one drive: rclone was asked the drive once" sh -c '[ "$(grep -c "^config/update driveid_final " "$1")" = 1 ]' x "$T/rc-real.log"
r=$(cmd signin onedrive "OneDrive 2")
cmd wait '"name":"OneDrive+2","kind"' 20 >/dev/null; r=$(cmd state)
check "a second account of the same kind is made too" has "$r" '"name":"OneDrive 2","kind"'
check "  and both are listed" has "$r" '"name":"OneDrive","kind"'
stop

run stale,real
r=$(cmd signin onedrive Work)
cmd wait '"phase":"done"' 20 >/dev/null; r=$(cmd state)
check "stale drive first: the sign-in finishes" has "$r" '"phase":"done"'
check "  the account is there" has "$r" '"name":"Work","kind"'
check "  the stale drive was tried, then the real one" sh -c 'grep -q "^config/update driveid_final id-stale" "$1" && grep -q "^config/update driveid_final id-real" "$1"' x "$T/rc-stale,real.log"
check "  rclone was sent back to choose_type in between" sh -c 'grep -q "^config/update choose_type -" "$1"' x "$T/rc-stale,real.log"
check "  the log says why" sh -c 'grep -q "choosing again" "$1"' x "$T/harness-stale,real.log"
stop

run none
r=$(cmd signin onedrive School)
cmd wait '"phase":"failed"' 20 >/dev/null; r=$(cmd state)
check "no drive: the sign-in fails" has "$r" '"phase":"failed"'
check "  with cloud.signinNoDrive" has "$r" '"error":"cloud.signinNoDrive"'
check "  and rclone's words" has "$r" '"message":"No drives found"'
check "  no account is left" has "$r" '"accounts":[]'
check "  its remote is deleted in rclone" sh -c 'grep -q "^config/delete" "$1"' x "$T/rc-none.log"
check "  no token in the state" hasnt "$r" 'AAAAAAAAAA'
stop

# 4. a password account's session (iCloud's cookies): rclone changes it in memory, lord-server keeps the newest in the
#    keyring and gives it back to rclone when it starts again (a logout, a reboot), so no new sign-in is asked for
run session
r=$(cmd add iclouddrive iCloud '{"apple_id":"someone@example.test","password":"pw-not-real"}')
id=$(printf '%s' "$r" | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')
cmd wait '"name":"iCloud"' 10 >/dev/null
cmd sleep 2500 >/dev/null                           # it is mounted once: rclone has a newer session
check "session: the account is made" sh -c '[ -n "$1" ]' x "$id"
cmd restart >/dev/null
cmd wait '"running":true' 10 >/dev/null; cmd sleep 1500 >/dev/null
r=$(cmd secret "$id")
check "session: before rclone stops, its newest session goes to the keyring" has "$r" '"cookies":"cookie-'
# the session rclone had when it was stopped: the last mount before the restart's config/create
last=$(sed -n '/^config\/create iclouddrive/,$p' "$T/rc-session.log" | awk '/^config\/create iclouddrive/{n++} n==1 && /^mount\/mount iclouddrive/{c=$3} END{sub("cookies=","",c); print c}')
check "  the cookie rclone had last" sh -c '[ -n "$2" ] && case $1 in *"\"cookies\":\"$2\""*) exit 0 ;; esac; exit 1' x "$r" "$last"
check "  rclone started again with it" sh -c 'grep "^config/create iclouddrive" "$1" | tail -1 | grep -q "cookies=$2\$"' x "$T/rc-session.log" "$last"
check "  not with the first one" hasnt "$(grep '^config/create iclouddrive' "$T/rc-session.log" | tail -1)" 'cookies=cookie-0'
check "  the session is never in rclone.conf" sh -c '[ -s "$1" ] && ! grep -q "cookie-\|trust-\|someone@" "$1"' x "$H/.config/lordos/cloud/rclone.conf"
stop

# 5. a saved session the provider refuses (rclone issue 9658): the cookies are dropped once, the trust token kept, and
#    the account mounts again without a new sign-in
run refused
sed -i 's/}$/,"session":"refused"}/' "$H/.fake-rclone.json"
r=$(cmd add iclouddrive iCloud '{"apple_id":"someone@example.test","password":"pw-not-real"}')
cmd wait '"name":"iCloud"' 10 >/dev/null; cmd sleep 2500 >/dev/null
check "refused session: the cookies are dropped once" sh -c '[ "$(grep -c "^config/update iclouddrive cookies=.." "$1")" = 1 ]' x "$T/rc-refused.log"
check "  then the mount goes on with a new session" sh -c 'grep -q "^mount/mount iclouddrive cookies=cookie-" "$1"' x "$T/rc-refused.log"
check "  the log says why" sh -c 'grep -q "refused its saved session" "$1"' x "$T/harness-refused.log"
stop

# 6. turned off and on by the person: rclone lets go of it, its record and sign-in stay, and on again it is given back to
#    rclone and mounted without a new sign-in; busy (a change waiting to go up), it asks, waits or is forced
run onoff
sed -i 's/}$/,"mounts":true}/' "$H/.fake-rclone.json"
r=$(cmd add iclouddrive Drive '{"apple_id":"someone@example.test","password":"pw-not-real"}')
id=$(printf '%s' "$r" | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')
cmd wait '"mounted":true' 10 >/dev/null
check "on/off: the account is mounted" has "$(cmd state)" '"mounted":true'
: >"$H/.fake-queue"; cmd sleep 2500 >/dev/null
r=$(cmd off "$id")
check "  busy: turning it off asks first" has "$r" '"error":"busy","files":1'
r=$(cmd off "$id" wait)
check "  Wait: it waits" has "$r" '"waiting":true'
check "  and stays on meanwhile" has "$(cmd state)" '"offWaiting":true'
rm -f "$H/.fake-queue"
cmd wait '"state":"off"' 10 >/dev/null; r=$(cmd state)
check "  once nothing waits, it is off" has "$r" '"state":"off","disabled":true,"offWaiting":false,"offReason":""'
check "  not mounted" has "$r" '"mounted":false'
check "  rclone let go of it" sh -c 'grep -q "^config/delete" "$1"' x "$T/rc-onoff.log"
check "  its sign-in is kept in the keyring" sh -c '[ -n "$1" ] && [ "$1" != null ]' x "$(cmd secret "$id")"
check "  its section stays in rclone.conf" grep -q "^\[$id\]" "$H/.config/lordos/cloud/rclone.conf"
check "  accounts.json says it is off" grep -q '"disabled":true' "$H/.config/lordos/cloud/accounts.json"
check "  its folder is gone from ~/Cloud" sh -c '[ ! -e "$1" ]' x "$H/Cloud/Drive"
stop
start
cmd sleep 2000 >/dev/null; r=$(cmd state)
check "  a new session: still off" has "$r" '"state":"off"'
check "  rclone never hears of it" sh -c '[ "$(grep -c "^config/create iclouddrive" "$1")" = 1 ]' x "$T/rc-onoff.log"
r=$(cmd on "$id")
check "  on again" has "$r" '"ok":true'
cmd wait '"mounted":true' 10 >/dev/null; r=$(cmd state)
check "  mounted again without a new sign-in" has "$r" '"mounted":true'
check "  with the sign-in it kept" sh -c 'grep "^config/create iclouddrive" "$1" | tail -1 | grep -q "cookies=cookie-"' x "$T/rc-onoff.log"
check "  not off" hasnt "$r" '"disabled":true'
r=$(cmd off "$id" force)
cmd wait '"state":"off"' 10 >/dev/null
check "  idle, off at once" has "$(cmd state)" '"state":"off"'
stop

# 7. rclone issues 9966 and 9658: a password provider that asks for a new code at the start of the session turns itself
#    off with its reason, without a sign-in prompt, its settings and sign-in kept; turned on by the person, it asks
run needcode
r=$(cmd add iclouddrive Phone '{"apple_id":"someone@example.test","password":"pw-not-real"}')
id=$(printf '%s' "$r" | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')
cmd wait '"name":"Phone"' 10 >/dev/null; cmd sleep 2500 >/dev/null
stop
before=$(cat "$H/.harness-keyring/$id" 2>/dev/null)
sed -i 's/}$/,"session":"needcode"}/' "$H/.fake-rclone.json"
start
cmd wait '"state":"off"' 15 >/dev/null; r=$(cmd state)
check "code asked at the start: the account turns itself off" has "$r" '"state":"off","disabled":true'
check "  with its reason" has "$r" '"offReason":"signinAtStart"'
check "  no sign-in is asked for" hasnt "$r" '"state":"signin"'
check "  the trust token was tried once" sh -c 'grep -q "^config/update iclouddrive code asked" "$1"' x "$T/rc-needcode.log"
check "  its sign-in in the keyring is untouched" sh -c '[ -n "$1" ] && [ "$1" = "$2" ]' x "$before" "$(cat "$H/.harness-keyring/$id" 2>/dev/null)"
check "  its section stays in rclone.conf" grep -q "^\[$id\]" "$H/.config/lordos/cloud/rclone.conf"
check "  the reason is kept" grep -q '"offReason":"signinAtStart"' "$H/.config/lordos/cloud/accounts.json"
check "  the log says why" sh -c 'grep -q "asks for a new sign-in at the start of the session" "$1"' x "$T/harness-needcode.log"
n=$(grep -c "^mount/mount iclouddrive" "$T/rc-needcode.log")
stop
start
cmd sleep 2000 >/dev/null
check "  the next session leaves it off: no mount tried" sh -c '[ "$(grep -c "^mount/mount iclouddrive" "$1")" = "$2" ]' x "$T/rc-needcode.log" "$n"
cmd on "$id" >/dev/null
cmd wait '"state":"signin"' 15 >/dev/null; r=$(cmd state)
check "  turned on by the person: the sign-in is asked for" has "$r" '"state":"signin"'
check "  and it stays on" hasnt "$r" '"disabled":true'
stop

echo "cloud-signin-fake: $pass passed, $fail failed"
[ "$fail" = 0 ]
