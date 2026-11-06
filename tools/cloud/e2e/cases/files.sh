#!/bin/sh
# files.sh USER BASE SERVER  (as root on the VM): cases/files.js on ~/Cloud/BASE, then checks the server's side:
# SERVER is a folder on this machine ("/srv/e2e/dav/files") or "mc:ALIAS/BUCKET" (MinIO keeps its own layout).
# Then cloud.delete of the test folder, and that nothing of it (and no .Trash-<uid>) stays on the server.
U=$1 BASE=$2 SRV=$3
D=$(cd "$(dirname "$0")/.." && pwd)
TAG=$(head -c 4 /dev/urandom | od -An -tx1 | tr -d ' \n')
H=$(getent passwd "$U" | cut -d: -f6)
[ -f "$H/e2e-local-2m.bin" ] || { head -c 2097152 /dev/urandom > "$H/e2e-local-2m.bin"; chown "$U": "$H/e2e-local-2m.bin"; }
sed -e "s|__BASE__|$BASE|g" -e "s|__TAG__|$TAG|g" "$D/cases/files.js" | python3 -I "$D/lc.py" "$U" - 900
sl() { case $SRV in mc:*) MC_CONFIG_DIR=/etc/lordos-e2e/mc /usr/local/lib/lordos-e2e/mc ls -r "${SRV#mc:}/$1" 2>&1 ;; *) ls -laR "$SRV/$1" 2>&1 ;; esac; }
echo "-- server side after the steps:"; sl "e2e-$TAG" | grep -v '^total' | head -20
want=$(sha256sum < "$H/e2e-local-2m.bin" | cut -c1-16)
case $SRV in mc:*) got=$(MC_CONFIG_DIR=/etc/lordos-e2e/mc /usr/local/lib/lordos-e2e/mc cat "${SRV#mc:}/e2e-$TAG/in.bin" | sha256sum | cut -c1-16) ;;
  *) got=$(sha256sum < "$SRV/e2e-$TAG/in.bin" | cut -c1-16) ;; esac
[ "$want" = "$got" ] && echo "SERVER in.bin hash ok" || echo "SERVER in.bin hash MISMATCH ($want vs $got)"
echo "-- cloud.delete of the folder:"
python3 -I "$D/lc.py" "$U" "const r = await cloud.delete([C('$BASE/e2e-$TAG')]); await sleep(8000); return r;" 120
sleep 3
echo "-- server side after delete:"; sl "e2e-$TAG" | head -3
case $SRV in mc:*) MC_CONFIG_DIR=/etc/lordos-e2e/mc /usr/local/lib/lordos-e2e/mc ls "${SRV#mc:}/" | grep -i trash ;; *) ls -a "$SRV" | grep -i trash ;; esac && echo "TRASH FOLDER ON SERVER" || echo "no .Trash on server"
