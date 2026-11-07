#!/bin/sh
# conflict-offline.sh USER BACKEND MODE (as root): the real offline conflict. BACKEND: dav sftp minio ftp smb rs3.
# MODE: diff (server edit of another size), same (same size), none (only our side changes: no cloud copy may appear, N5).
# Base up → the server process stopped and its sessions killed → our edit (rclone retries meanwhile) → the server's
# copy edited (on disk while down; MinIO right after it comes back, its data isn't plain files) → server started →
# what each side holds.
U=$1 B=$2 MODE=${3:-diff}
D=$(cd "$(dirname "$0")/.." && pwd)
S=$D/servers.sh
MC="env MC_CONFIG_DIR=/etc/lordos-e2e/mc /usr/local/lib/lordos-e2e/mc"
TAG=$(head -c 3 /dev/urandom | od -An -tx1 | tr -d ' \n'); F=off-$TAG.txt
case $B in
dav)   ACC=DavHttp;            DIR=/srv/e2e/dav/files; OWN=www-data
       stop() { systemctl stop apache2; pkill -9 -x apache2; }; start() { systemctl start apache2; } ;;
sftp)  ACC=SftpPw;             DIR=/home/e2epw; OWN=e2epw
       stop() { systemctl stop e2e-sshd; pkill -9 -u e2epw; }; start() { sh $S start-one sshd; } ;;
minio) ACC=MinIO/e2e-alpha;    DIR=mc:e2e/e2e-alpha
       stop() { systemctl stop e2e-minio; }; start() { sh $S start-one minio; for i in $(seq 1 20); do curl -s -o /dev/null http://127.0.0.1:9000/minio/health/live && break; sleep 0.5; done; } ;;
ftp)   ACC=FTP;                DIR=/home/e2epw; OWN=e2epw
       stop() { systemctl stop e2e-ftp; pkill -9 -x vsftpd; }; start() { sh $S start-one ftp; } ;;
smb)   ACC=SMB/e2e;            DIR=/srv/e2e/smb; OWN=e2epw
       stop() { systemctl stop e2e-smb; pkill -9 -x smbd; }; start() { sh $S start-one smb; } ;;
rs3)   ACC=RcloneS3/bucket-a;  DIR=/srv/e2e/rs3/bucket-a; OWN=rs3-e2e
       stop() { systemctl stop e2e-rs3; }; start() { sh $S start-one rs3; } ;;
esac
put() { case $DIR in mc:*) printf '%s' "$1" > /tmp/cfo.$$; $MC cp -q /tmp/cfo.$$ "${DIR#mc:}/$F" >/dev/null ;; *) printf '%s' "$1" | sudo -u $OWN tee "$DIR/$F" >/dev/null ;; esac; }
lc() { python3 -I "$D/lc.py" "$U" "$1" "${2:-90}"; }
case $MODE in same) OURS='ours offline v1'; THEIRS='srvr offline v1' ;; *) OURS='ours, changed on this computer while offline'; THEIRS='the server changed it meanwhile' ;; esac
lc "const f = C('$ACC/$F'); lord.fs.write(f, 'base v0'); return await until(() => (cloud.status([f])[f] || {}).st === 'here', 60000);" >/dev/null
sleep 4
stop; sleep 1
lc "lord.fs.write(C('$ACC/$F'), '$OURS'); await sleep(15000); return [A('${ACC%%/*}').state, cloud.status([C('$ACC/$F')])];" 60
[ "$MODE" != none ] && case $DIR in mc:*) ;; *) put "$THEIRS" ;; esac
sleep 2; start
[ "$MODE" != none ] && case $DIR in mc:*) put "$THEIRS" ;; esac
lc "await until(() => { const a = A('${ACC%%/*}'); return a.state === 'ok' && a.up.files === 0; }, 180000); await sleep(5000); return { state: A('${ACC%%/*}').state, conflicts: (S().conflicts || []).filter(c => c.path.includes('$TAG')).map(c => c.kept.replace(home, '~')), here: lord.fs.list(C('$ACC')).map(e => e.name).filter(n => n.includes('$TAG')) };" 240
get() { case $DIR in mc:*) $MC cat "${DIR#mc:}/$1" 2>&1 ;; *) cat "$DIR/$1" 2>&1 ;; esac; }
L=$(case $DIR in mc:*) $MC ls "${DIR#mc:}/" | grep "$TAG" | sed 's/^.*STANDARD //' ;; *) ls "$DIR" | grep "$TAG" ;; esac)
printf '%s\n' "$L" | while IFS= read -r k; do [ -n "$k" ] && echo "   server: $k = $(get "$k")"; done
