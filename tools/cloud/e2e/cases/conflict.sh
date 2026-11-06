#!/bin/sh
# conflict.sh USER ACCOUNT_REL SERVER SAMESIZE (as root): a file changed on both sides while this computer's change waits
# (the account paused: its upload is held as when offline), then Resume. SERVER as in files.sh. SAMESIZE=1 keeps the size.
U=$1 REL=$2 SRV=$3 SAME=${4:-0}
D=$(cd "$(dirname "$0")/.." && pwd)
TAG=$(head -c 3 /dev/urandom | od -An -tx1 | tr -d ' \n'); F=conf-$TAG.txt
put() { case $SRV in mc:*) printf '%s' "$1" > /tmp/conf.$$; MC_CONFIG_DIR=/etc/lordos-e2e/mc /usr/local/lib/lordos-e2e/mc cp -q /tmp/conf.$$ "${SRV#mc:}/$F" >/dev/null ;;
  *) o=$(stat -c %U "$SRV"); printf '%s' "$1" | sudo -u "$o" tee "$SRV/$F" >/dev/null ;; esac; }
get() { case $SRV in mc:*) MC_CONFIG_DIR=/etc/lordos-e2e/mc /usr/local/lib/lordos-e2e/mc cat "${SRV#mc:}/$1" 2>&1 ;; *) cat "$SRV/$1" 2>&1 ;; esac; echo; }
python3 -I "$D/lc.py" "$U" "const f = C('$REL/$F'); lord.fs.write(f, 'base v0'); return await until(() => (cloud.status([f])[f] || {}).st === 'here', 60000);" 90 >/dev/null
sleep 3
python3 -I "$D/lc.py" "$U" "const a = A('${REL%%/*}'); cloud.pause(a.id); lord.fs.write(C('$REL/$F'), '$( [ "$SAME" = 1 ] && echo 'ours v1' || echo 'ours, this computer v1')'); return 1;" 30 >/dev/null
sleep 2
if [ "$SAME" = 1 ]; then put 'srvr v1'; else put 'the server changed it, cloud v1'; fi
sleep 2
python3 -I "$D/lc.py" "$U" "const a = A('${REL%%/*}'); cloud.resume(a.id); const f = C('$REL/$F'); await until(() => (cloud.status([f])[f] || {}).st === 'here', 90000); await sleep(4000); return { conflicts: (S().conflicts || []).filter(c => c.path.includes('$F')).map(c => ({ path: c.path.replace(home, '~'), kept: c.kept.replace(home, '~') })), list: lord.fs.list(C('$REL')).map(e => e.name).filter(n => n.includes('$TAG')) };" 150
echo "-- server: $F = $(get "$F")"
case $SRV in mc:*) K=$(MC_CONFIG_DIR=/etc/lordos-e2e/mc /usr/local/lib/lordos-e2e/mc ls "${SRV#mc:}/" | grep "$TAG" | sed 's/^.*STANDARD //' | grep -vx "$F") ;; *) K=$(ls "$SRV" | grep "$TAG" | grep -vx "$F") ;; esac
printf '%s\n' "$K" | while IFS= read -r k; do [ -n "$k" ] && echo "-- server: $k = $(get "$k")"; done
[ -z "$K" ] && echo "-- server: no other copy"
