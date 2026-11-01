#!/bin/bash
# share_e2e.sh — the screen-sharing API against real, isolated headless sessions (docs/SCREEN-SHARING.md). A test
# machine only, with sudo; not part of make test. Run from the repository root after `make -C server build/share-client`:
#   sudo needs no password here; it makes one app scope, writes the user's grants file and removes both at the end.
# It never touches a running LordOS session: its sessions live in ./rt with their own HOME.
U=$(id -u); ROOT=$PWD; B=$ROOT/server/build; C=$B/share-client; R=$ROOT/rt; RG=$ROOT/rtg
CGB=/sys/fs/cgroup/system.slice/lordos.service/lordos-session-$U; CG=$CGB/app-lordos-sharetest.scope
G=/var/lib/lordos/share/$U.json; ID=desktop:sharetest
pass=0; fail=0
ok() { pass=$((pass+1)); echo "ok   $1"; }
no() { fail=$((fail+1)); echo "FAIL $1"; }
check() { local n=$1; shift; if out=$("$@" 2>&1); then ok "$n"; else no "$n: $out"; fi; }
fails() { local n=$1; shift; if out=$("$@" 2>&1); then no "$n (it worked): $out"; else ok "$n"; fi; }
cat > $ROOT/inscope.sh <<X
#!/bin/sh
echo \$\$ > $CG/cgroup.procs
exec setpriv --reuid=$U --regid=$(id -g) --init-groups "\$@"
X
chmod +x $ROOT/inscope.sh
app() { sudo $ROOT/inscope.sh $C api $R/lordos/share.sock "$@"; }
out() { $C api $R/lordos/share.sock "$@"; }
rootm() { sudo $C mirror $R/lordos/remote.sock "$@"; }
grant() { sudo mkdir -p -m755 /var/lib/lordos/share; printf '%s' "$1" | sudo tee $G.t >/dev/null; sudo chown root:$(id -g) $G.t; sudo chmod 640 $G.t; sudo mv $G.t $G; }
allow() { grant "{\"v\":1,\"grants\":[{\"id\":\"$ID\",\"exe\":\"$C\",\"perm\":\"screen-share\",\"state\":\"allow\"},{\"id\":\"$ID\",\"exe\":\"$C\",\"perm\":\"remote-input\",\"state\":\"allow\"}]}"; }
session() {
  mkdir -p -m700 $1 $ROOT/home
  env -i HOME=$ROOT/home XDG_RUNTIME_DIR=$1 PATH=$PATH LANG=en_US.UTF-8 LORD_GPU=0 $B/lord-server --headless 1280x800 $2 --theme $3 --fonts assets/fonts --shell assets/shell > $4 2>&1 &
  echo $!
}
cleanup() { kill $S $SG 2>/dev/null; sudo pkill -f "share-client mirror .* wait 60000[0]"; sudo rm -f $G; sleep 0.5; sudo rmdir $CG $CGB 2>/dev/null; rm -f $ROOT/inscope.sh; }
trap cleanup EXIT
sudo mkdir -p $CG; sudo rm -f $G

# ---- the login screen: no socket at all
SG=$(session $RG --greeter themes/greeter/theme.bundle.js greeter.log); sleep 3
if [ -e $RG/lordos/remote.sock ] && [ ! -e $RG/lordos/share.sock ]; then ok "login screen: no share.sock"; else no "login screen: no share.sock"; fi
kill $SG; SG=

S=$(session $R "" themes/default/theme.bundle.js sess.log); sleep 4
[ -S $R/lordos/share.sock ] && ok "session: share.sock exists" || no "session: share.sock exists"
[ "$(stat -c %a $R/lordos/share.sock)" = 600 ] && ok "share.sock is 0600" || no "share.sock is 0600"

# ---- identity, and the root-only session socket (commit 2825555c)
check "no app scope: no_identity, closed" out gone
check "remote.sock: a process of the same user is closed at once" $C mirror $R/lordos/remote.sock gone
check "remote.sock: root is served (RP_SIZE)" rootm until 1 1500
check "remote.sock: a bridge's order works: connect as root, then drop to the user" sudo $B/share-client bridge $R/lordos/remote.sock $U

# ---- grants
check "no grant, nobody to ask: no_person" app start 1 1 2000 expect-ref no_person 2000
grant "{\"v\":1,\"grants\":[{\"id\":\"$ID\",\"perm\":\"screen-share\",\"state\":\"deny\"}]}"
check "denied" app start 1 1 2000 expect-ref denied 2000
grant "{\"v\":1,\"grants\":[{\"id\":\"$ID\",\"exe\":\"/usr/bin/other\",\"perm\":\"screen-share\",\"state\":\"allow\"}]}"
check "a grant for another program asks again: no_person" app start 1 1 2000 expect-ref no_person 2000
grant "{broken"
check "a damaged file grants nothing" app start 1 1 2000 expect-ref no_person 2000
allow; sudo chmod 666 $G
check "a file others can write grants nothing" app start 1 1 2000 expect-ref no_person 2000
allow

# ---- a share: picture, one holder, input
check "share: H.264 frames arrive after the indicator" app start 1 1 2000 until 60 3000 until 1 1000 until 40 4000 stop expect-end ended_by_app 1000
check "share: VP8" app start 1 2 2000 until 40 4000 stop expect-end ended_by_app 1000
sleep 1
Z=$(ps -eo stat,comm | grep -c '^Z.*lord-media')
[ "$Z" = 0 ] && ok "lord-media: no zombie 1 s after stopping" || no "lord-media: $Z zombie(s)"
pgrep -f "lord-media" -u $U >/dev/null && no "lord-media still running after the share" || ok "encoder gone after the share"

app start 5 1 2000 until 40 4000 wait 3000 stop > a.out 2>&1 &
AP=$!
sleep 1.5
check "second app: busy" app start 1 1 2000 expect-ref busy 1000
wait $AP

# input reaches the session through a share, and not without one
check "input: a pointer move comes back as RP_CURSOR" app start 5 1 2000 until 40 4000 ptr 321 123 until 4 2000 stop
check "input: refused with no share" app ptr 11 11 none 4 500
grant "{\"v\":1,\"grants\":[{\"id\":\"$ID\",\"exe\":\"$C\",\"perm\":\"screen-share\",\"state\":\"allow\"}]}"
check "no remote-input grant: frames yes, input no" app start 5 1 2000 until 40 4000 reset ptr 400 400 none 4 800 stop
allow

# ---- the lock: no picture, no input, until lord-sessiond lends the monitor
L0=$(grep -c 'locked' sess.log)
check "lock: SHARE_END locked, then no frame and no cursor" app start 5 1 2000 until 40 4000 reset lock expect-end locked 1000 ptr 500 500 none 40 2500
check "locked: start refused" app start 1 1 2000 expect-ref locked 1000
check "locked: nothing reaches a client that keeps sending" app query until 60 1000 ptr 600 600 key 30 1 key 30 0 none 40 500
check "locked: viewing and input stay refused 2 s later" app start 5 1 2000 expect-ref locked 1000 none 40 2000
sleep 1
pgrep -f lord-media -u $U >/dev/null && no "locked: an encoder is running" || ok "locked: the encoder stopped"
# what a 5673 viewer would see behind the lock: the socket itself keeps giving frames to root
if out=$(rootm video 1 2000 until 40 3000 2>&1); then echo "note 5673: remote.sock still gives frames to a root client while locked (frames: yes)"; else echo "note 5673: no frames to a root client while locked: $out"; fi
check "lock stays after a seat leaves" rootm seat undisplay wait 200
check "still locked after the monitor leaves" app start 1 1 2000 expect-ref locked 1000
seat_up() { rootm seat display wait 600000 >/dev/null 2>&1 & SP=$!; sleep 0.8; }
seat_down() { sudo pkill -f "share-client mirror .* seat display wait 60000[0]"; wait $SP 2>/dev/null; }
seat_up; ok "a seat lends the monitor"
check "share again after the unlock" app start 1 1 2000 until 40 4000 stop

# ---- the monitor taken from the session, or its seat gone
app start 1 1 2000 until 40 4000 expect-end away 5000 > a.out 2>&1 &
AP=$!; sleep 1.5; rootm seat undisplay wait 300 >/dev/null
if wait $AP; then ok "the monitor leaves: SHARE_END away"; else no "the monitor leaves: $(cat a.out)"; fi
check "away: start refused" app start 1 1 2000 expect-ref away 1000
seat_down; seat_up
app start 1 1 2000 until 40 4000 expect-end away 5000 > a.out 2>&1 &
AP=$!
sleep 1.5
seat_down
if wait $AP; then ok "lord-sessiond's connection ends: SHARE_END away"; else no "seat gone: $(cat a.out)"; fi
seat_up

# ---- one door at a time (5673's viewer is a root connection with RP_HELLO)
rootm hello "10.0.0.9 lord" wait 2500 > m.out 2>&1 &
AP=$!
sleep 1
check "5673 connected: refused remote_desktop" app start 1 1 2000 expect-ref remote_desktop 1000
check "5673 connected: SHARE_QUERY says so too" app query until 60 1000
wait $AP
sleep 0.3
check "5673 gone: the door is free at once" app start 1 1 2000 until 40 4000 stop
rootm hello "10.0.0.9 lord" video 1 2000 until 40 3000 > /dev/null 2>&1 && ok "no share: a viewer gets its frames as before" || no "no share: a viewer gets its frames as before"
app start 1 1 2000 until 40 4000 wait 3500 until 40 4000 stop > a.out 2>&1 &
AP=$!
sleep 1.5
check "share active: a 5673 sign-in gets RP_BUSY" rootm hello "10.0.0.9 lord" until 44 1000 video 1 2000 none 40 1000
if wait $AP; then ok "the share didn't notice the refused sign-in"; else no "share disturbed: $(cat a.out)"; fi
sleep 0.3
check "share ended: 5673 comes in" rootm hello "10.0.0.9 lord" video 1 2000 until 40 3000

# ---- revoking
app start 1 1 2000 until 40 4000 expect-end revoked 3000 > a.out 2>&1 &
AP=$!
sleep 1.5; T0=$(date +%s%N); sudo rm -f $G
if wait $AP; then ok "revoked: ended after $(( ($(date +%s%N)-T0)/1000000 )) ms"; else no "revoke: $(cat a.out)"; fi
allow

# ---- keyframes on a still screen (requests every second for 12 s)
sleep 1
A="start 1 1 2000 until 40 4000 wait 1500 reset"; for i in 1 2 3 4 5 6 7 8 9 10 11 12; do A="$A keyframe wait 1000"; done
app $A stop > k.out 2>&1
K=$(sed -n 's/^type 255: \([0-9]*\).*/\1/p' k.out); K=${K:-0}
[ "$K" -le 3 ] && ok "still screen: 12 requests, $K keyframes" || no "still screen: $K keyframes in 12 s"

# ---- the clipboard
rootm clip "before the share" wait 100 >/dev/null
check "clipboard: what was copied before doesn't go" app start 13 1 2000 until 40 4000 none 6 800 stop
app start 13 1 2000 until 40 4000 until 6 4000 stop > a.out 2>&1 &
AP=$!
sleep 1.5; rootm clip "after the start" wait 100 >/dev/null
if wait $AP; then ok "clipboard: what is copied afterwards goes"; else no "clipboard after: $(cat a.out)"; fi

# ---- malformed input on a live connection
check "a zero length ends the connection" app junk gone
check "a seat message on this door ends it" app raw 18 0 gone
check "RP_DISPLAY on this door ends it" app raw 19 1 gone
check "RP_KEYRING on this door ends it" app raw 24 8 gone
check "a wrong length ends it" app raw 50 7 gone
check "an unknown type ends it" app raw 199 0 gone
A=0; for i in $(seq 1 200); do head -c $((RANDOM%4096+1)) /dev/urandom | timeout 2 sudo $ROOT/inscope.sh python3 -c "
import socket,sys
s=socket.socket(socket.AF_UNIX); s.connect('$R/lordos/share.sock'); s.sendall(sys.stdin.buffer.read())" 2>/dev/null; kill -0 $S 2>/dev/null && A=$((A+1)); done
[ "$A" = 200 ] && ok "200 connections of random bytes: the session lives" || no "random bytes: alive after $A of 200"
check "the API still answers after the noise" app start 1 1 2000 until 40 4000 stop

# ---- the end
app start 1 1 2000 until 40 4000 wait 8000 > /dev/null 2>&1 &
sleep 1.5; kill $S; sleep 1.5; [ -e $R/lordos/share.sock ] && no "exit: the socket stays" || ok "exit: the socket is removed"
seat_down; S=
echo "share-e2e: $pass passed, $fail failed"
grep -c 'share:' sess.log
exit $((fail != 0))
