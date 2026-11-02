#!/bin/bash
# companion_e2e.sh <fixture> — a packaged app and its companion in real, isolated headless sessions (docs/GO-LINK.md). A test
# machine only, with sudo; not part of make test. From the repository root, after `make -C server build/lord-server-pkgtest
# build/share-client` and tests/companion/fixture.sh. It runs build/lord-server-pkgtest, which takes its official key and third
# app root from the environment; the root is a root-owned folder here, removed at the end. It never touches a running session.
F=$(cd "$1" && pwd); ROOT=$PWD; B=$ROOT/server/build; U=$(id -u)
T=$ROOT/ct; R=$T/rt; H=$T/home; APPS=$T/apps; G=/var/lib/lordos/share/$U.json
pass=0; fail=0
ok() { pass=$((pass+1)); echo "ok   $1"; }
no() { fail=$((fail+1)); echo "FAIL $1"; }
is() { if [ "$2" = "$3" ]; then ok "$1"; else no "$1: got '$2', wanted '$3'"; fi; }
has() { if grep -q -- "$2" "$3" 2>/dev/null; then ok "$1"; else no "$1: no '$2' in $3"; fi; }
hasnt() { if grep -q -- "$2" "$3" 2>/dev/null; then no "$1: '$2' in $3"; else ok "$1"; fi; }
gone() { if [ ! -e "$2" ]; then ok "$1"; else no "$1: $2 exists"; fi; }
OFF=$(cat $F/official.hex)
export LORD_TEST_OFFICIAL=$OFF LORD_TEST_APPS_ROOT=$APPS
S=
stop() { [ -n "$S" ] && kill $S 2>/dev/null; pkill -f '^companion$'; sleep 0.7; S=; }
cleanup() { stop; sudo rm -rf $T/apps; sudo rm -f $G; rm -rf $T; }
trap cleanup EXIT
sudo rm -rf $T; mkdir -p -m700 $T $R; sudo rm -f $G

install() { sudo rm -rf $APPS; sudo mkdir -p $APPS; sudo cp -r $F/pkgtest $APPS/; sudo chown -R root:root $APPS; sudo chmod -R go-w $APPS; }
catalog() { mkdir -p $H/.local/share/lordos/catalog; cp $F/cat-$1/catalog.json $F/cat-$1/catalog.sig $H/.local/share/lordos/catalog/; }
freshhome() {
  rm -rf $H; mkdir -p $H/.config/lordos $H/Downloads
  echo secret > $H/probe-home.txt
  echo secret > $H/.probe-dot; mkdir -p $H/.probe-hidden $H/Documents $H/.local/share/lordos/appdata/other; echo secret > $H/.probe-hidden/secret
  ln -s .probe-hidden/secret $H/link-to-hidden
  cat > $H/.config/lordos/permisos.json <<X
{ "version": 1, "apps": { "pkgtest": { "network": { "state": "allow", "when": 1 }, "status-item": { "state": "allow", "when": 1 } } } }
X
  echo '{ "loginApps": ["lpk:pkgtest"] }' > $H/.config/lordos/settings.json     # the shell opens it, as Open at Login does
  catalog ${1:-good}
}
grant() { sudo mkdir -p -m755 /var/lib/lordos/share; printf '%s' "$1" | sudo tee $G.t >/dev/null; sudo chown root:$(id -g) $G.t; sudo chmod 640 $G.t; sudo mv $G.t $G; }

session() {
  rm -f $H/.cache/lordos/session.log
  (cd $ROOT/server && exec env -i HOME=$H XDG_RUNTIME_DIR=$R PATH=$PATH LANG=en_US.UTF-8 LORD_GPU=0 LORD_TEST_OFFICIAL=$OFF LORD_TEST_APPS_ROOT=$APPS \
    $B/lord-server-pkgtest --headless 1280x800 --theme ../themes/default/theme.bundle.js --fonts ../assets/fonts --shell ../assets/shell > /dev/null 2>&1) &
  S=$!
}
wait_for() { local i=0; while [ $i -lt ${2:-120} ] && [ ! -e "$1" ]; do sleep 0.25; i=$((i+1)); done; [ -e "$1" ]; }
cpid() { pgrep -f '^companion$' | head -1; }
DATA=$H/.local/share/lordos/appdata/pkgtest; REP=$H/Downloads/companion-report.txt

# a direct start, as lord-server does it: fd 3 a SEQPACKET socket. Prints what the app would hear first.
direct() {
  python3 - "$B/lord-server-pkgtest" "$@" <<'X'
import os,socket,subprocess,sys
a,b=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET)
def child():
    os.dup2(b.fileno(),3)
p=subprocess.Popen([sys.argv[1],'--companion',sys.argv[2],'0'],preexec_fn=child,pass_fds=(3,) if False else (),close_fds=False,stderr=subprocess.DEVNULL)
b.close()
a.settimeout(8)
try: print(a.recv(4096).decode())
except Exception as e: print('nothing',e)
a.close(); p.wait(timeout=8)
X
}
dhome() { env HOME=$H LORD_TEST_OFFICIAL=$OFF LORD_TEST_APPS_ROOT=$APPS "$@"; }

echo "== the package, as signed"
install; freshhome
grant "{\"v\":1,\"grants\":[{\"id\":\"lpk:pkgtest\",\"exe\":\"\",\"perm\":\"screen-share\",\"state\":\"allow\"},{\"id\":\"lpk:pkgtest\",\"exe\":\"\",\"perm\":\"remote-input\",\"state\":\"allow\"}]}"
session; wait_for $REP 160 || no "the companion wrote its report"
sleep 1
cat $REP > $T/report.txt
grep -c '^ok' $T/report.txt | sed 's/^/inside the sandbox, passed: /'
grep '^FAIL' $T/report.txt
pc=$(grep -c '^ok' $T/report.txt); fc=$(grep -c '^FAIL' $T/report.txt); pass=$((pass+pc)); fail=$((fail+fc))
has "the report is complete" '^end' $T/report.txt
has "fd 4: frames with a grant" '^share: frames' $T/report.txt
has "SHARE_QUERY shows the grants before the first start" '^share: query granted 15' $T/report.txt
has "the app heard it started" '"started"' $DATA/msg0.json
wait_for $DATA/msg1.json 20; has "fd 3 both ways" 'pong' $DATA/msg1.json
has "the status item was accepted" '"ok":true' $DATA/status.json
P=$(cpid)
if [ -n "$P" ]; then
  is "no_new_privs" "$(awk '/NoNewPrivs/{print $2}' /proc/$P/status)" 1
  is "seccomp filter" "$(awk '/^Seccomp:/{print $2}' /proc/$P/status)" 2
  is "no capabilities" "$(awk '/CapEff/{print $2}' /proc/$P/status)" 0000000000000000
  is "not root" "$(awk '/^Uid/{print $2}' /proc/$P/status)" $U
  is "one process, no children" "$(pgrep -P $P | wc -l)" 0
else no "the companion runs"; fi
hasnt "lord-server didn't log a refusal" 'not started' $H/.cache/lordos/session.log
hasnt "the shell took the status item without an error" 'TypeError\|ReferenceError' $H/.cache/lordos/session.log

echo "== it stops with the app"
A=$(pgrep -x pkgtest | head -1)
[ -n "$A" ] && kill -9 $A
sleep 1.5
is "killed with the app" "$(cpid)" ""
is "no zombie left" "$(ps --ppid $S -o stat= 2>/dev/null | grep -c Z)" 0
stop

echo "== a clean exit: the end of fd 3"
freshhome; session; wait_for $REP 160
A=$(pgrep -x pkgtest | head -1); P=$(cpid)
kill -TERM $A; sleep 1.5
is "the app's TERM ends both" "$(cpid)" ""
stop

echo "== no grant: fd 4 follows the rules"
sudo rm -f $G
freshhome; session; wait_for $REP 160; sleep 0.5
has "no grant, nobody to ask: no_person" '^share: refused 3' $REP
stop

echo "== a changed byte refuses to start"
install; freshhome
sudo python3 -c "
f=open('$APPS/pkgtest/bin/engine','r+b'); f.seek(4096); b=f.read(1); f.seek(4096); f.write(bytes([b[0]^1]))"
x=$(dhome bash -c "$(declare -f direct); B=$B direct pkgtest"); case "$x" in *refused*sig.fileModified*) ok "--companion: one byte: refused ($x)";; *) no "one byte: $x";; esac
gone "nothing of it ran" $REP
session; sleep 4
has "the app itself is refused too" "was refused: sig.fileModified" $H/.cache/lordos/session.log
gone "no report" $REP
is "no companion" "$(cpid)" ""
stop

echo "== a key the catalog doesn't list; a revoked key; no catalog"
install; freshhome none
x=$(dhome bash -c "$(declare -f direct); B=$B direct pkgtest"); case "$x" in *refused*notVerified*) ok "not a Verified author";; *) no "not a Verified author: $x";; esac
freshhome revoked
x=$(dhome bash -c "$(declare -f direct); B=$B direct pkgtest"); case "$x" in *refused*sig.revokedKey*) ok "revoked";; *) no "revoked: $x";; esac
freshhome; rm -rf $H/.local/share/lordos/catalog
x=$(dhome bash -c "$(declare -f direct); B=$B direct pkgtest"); case "$x" in *refused*notVerified*) ok "no catalog";; *) no "no catalog: $x";; esac
gone "nothing of it ran" $REP

echo "== the folder must be root's"
freshhome; sudo chown -R $U $APPS
x=$(dhome bash -c "$(declare -f direct); B=$B direct pkgtest"); case "$x" in *refused*notInstalled*) ok "a folder of the user's: refused";; *) no "a user folder: $x";; esac
install; sudo chmod g+w $APPS/pkgtest
x=$(dhome bash -c "$(declare -f direct); B=$B direct pkgtest"); case "$x" in *refused*notInstalled*) ok "a group-writable folder: refused";; *) no "group-writable: $x";; esac
install
x=$(sudo env HOME=$H LORD_TEST_OFFICIAL=$OFF LORD_TEST_APPS_ROOT=$APPS bash -c "$(declare -f direct); B=$B direct pkgtest"); case "$x" in *refused*companion.root*) ok "as root: refused";; *) no "as root: $x";; esac
sudo rm -f $T/home/Downloads/companion-report.txt

echo "== a link for Downloads isn't followed"
freshhome; rmdir $H/Downloads; ln -s /tmp $H/Downloads
x=$(dhome bash -c "$(declare -f direct); B=$B direct pkgtest"); case "$x" in *refused*noDownloads*) ok "refused";; *) no "Downloads a link: $x";; esac

echo "== the same manifest in a home .lpk is ignored"
sudo rm -rf $APPS; sudo mkdir -p $APPS; sudo chmod 755 $APPS
freshhome; mkdir -p $H/.local/share/lordos/apps; cp -r $F/pkgtest $H/.local/share/lordos/apps/; chmod -R u+w $H/.local/share/lordos/apps
session; sleep 5
is "the home app runs" "$(pgrep -x pkgtest | wc -l)" 1
is "no companion" "$(cpid)" ""
gone "no report" $REP
has "the app heard no" '"refused"' $DATA/msg0.json
stop
rm -rf $H/.local/share/lordos/apps
gone "(clean)" $H/.local/share/lordos/apps

echo "== a Linux app whose scope reads as a package gets nothing"
CGB=/sys/fs/cgroup/system.slice/lordos.service/lordos-session-$U; CG=$CGB/app-lordos-lpk-pkgtest.scope
freshhome; session; sleep 3
if sudo mkdir -p $CG 2>/dev/null; then
  cat > $T/inscope.sh <<X
#!/bin/sh
echo \$\$ > $CG/cgroup.procs
exec setpriv --reuid=$U --regid=$(id -g) --init-groups "\$@"
X
  chmod +x $T/inscope.sh
  if sudo $T/inscope.sh $B/share-client api $R/lordos/share.sock gone; then ok "lpk scope on the socket: no_identity"; else no "lpk scope on the socket"; fi
  sleep 0.3; sudo rmdir $CG
else echo "skipped: no session cgroup here"; fi
stop

echo "passed $pass, failed $fail"
[ $fail = 0 ]
