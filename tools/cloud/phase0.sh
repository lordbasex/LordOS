#!/bin/sh
# Cloud Phase 0 probe (docs/CLOUD.md section 14): rcd on a Unix socket, the rc calls, a mount through rc,
# eviction while mounted, OAuth config states. Runs as a normal user on Debian 13 with fuse3.
set -u
B=${1:-$HOME/cloud0/rclone-amd64}
W=$HOME/cloud0/run; rm -rf "$W"; mkdir -p "$W/src" "$W/mnt" "$W/cache" "$W/rt"; chmod 700 "$W/rt"
SOCK=$W/rt/rc.sock U=u$(head -c8 /dev/urandom | od -An -tx1 | tr -d ' \n') P=$(head -c24 /dev/urandom | od -An -tx1 | tr -d ' \n')
say() { printf '\n== %s\n' "$*"; }
rc() { "$B" rc --unix-socket "$SOCK" --rc-user "$U" --rc-pass "$P" "$@"; }

say version; "$B" version | head -1

# a local WebDAV server as the "cloud" (loopback, its own user)
mkdir -p "$W/src/Projects"; head -c 5000000 /dev/urandom >"$W/src/Projects/big.bin"; echo hello >"$W/src/notes.txt"
"$B" serve webdav "$W/src" --addr 127.0.0.1:18080 --user dav --pass davpass >"$W/dav.log" 2>&1 &
DAV=$!

say "rcd on a Unix socket"
"$B" rcd --rc-addr "unix://$SOCK" --rc-user "$U" --rc-pass "$P" --config "$W/rclone.conf" --cache-dir "$W/cache" >"$W/rcd.log" 2>&1 &
RCD=$!
for i in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.1; done
ls -l "$SOCK"; stat -c 'socket mode %a' "$SOCK"
say "TCP listeners of rcd (expect none)"; ss -ltnpH 2>/dev/null | grep "pid=$RCD," || echo "none"
say "no auth is refused"; "$B" rc --unix-socket "$SOCK" operations/list fs=/ remote=tmp 2>&1 | tail -2
say "rc/noop"; rc rc/noop a=1

say "the calls docs/CLOUD.md section 5 needs"
rc rc/list >"$W/rclist.json"
for m in config/providers config/create config/update config/delete config/get mount/mount mount/unmount mount/listmounts operations/about operations/publiclink core/stats core/bwlimit vfs/stats vfs/queue vfs/refresh vfs/forget vfs/list sync/copy sync/sync sync/bisync job/status job/stop core/quit core/command config/dump; do
  grep -q "\"Path\": \"$m\"" "$W/rclist.json" && echo "have $m" || echo "MISSING $m"
done

say "config/create webdav (password obscured by rclone)"
rc config/create name=nas type=webdav parameters='{"url":"http://127.0.0.1:18080","vendor":"other","user":"dav","pass":"davpass"}' opt='{"obscure":true,"nonInteractive":true}' | head -5
echo "-- rclone.conf:"; sed 's/pass = .*/pass = <obscured>/' "$W/rclone.conf"

say "operations/about on webdav"; rc operations/about fs=nas: 2>&1 | head -8

say "mount/mount with full VFS cache, no eviction by rclone"
rc mount/mount fs=nas: mountPoint="$W/mnt" vfsOpt='{"CacheMode":"full","CacheMaxAge":3153600000000000000,"WriteBack":5000000000}' 2>&1 | tail -2
sleep 1; ls -la "$W/mnt" "$W/mnt/Projects"
rc mount/listmounts

say "read through the mount: comes into the cache"
t0=$(date +%s%N); sha_src=$(sha256sum <"$W/src/Projects/big.bin" | cut -c1-16); sha1=$(sha256sum <"$W/mnt/Projects/big.bin" | cut -c1-16); t1=$(date +%s%N)
echo "src $sha_src mount $sha1 ($(( (t1-t0)/1000000 )) ms)"
find "$W/cache" -type f | sed "s|$W/cache/||"
rc vfs/stats fs=nas: | head -30

say "evict while mounted: remove the cache file and its metadata, then read again"
DATA=$(find "$W/cache/vfs" -name big.bin -type f); META=$(find "$W/cache/vfsMeta" -name big.bin -type f)
echo "data $DATA"; echo "meta $META"; cat "$META"; echo
rm -f "$DATA" "$META"
sha2=$(sha256sum <"$W/mnt/Projects/big.bin" | cut -c1-16); echo "after evict: $sha2 (want $sha_src)"
ls -la "$DATA" 2>&1 | tail -1
say "evict variant: vfs/forget then remove"
rm -f "$DATA" "$(find "$W/cache/vfsMeta" -name big.bin -type f)"; rc vfs/forget fs=nas: file=Projects/big.bin
sha3=$(sha256sum <"$W/mnt/Projects/big.bin" | cut -c1-16); echo "after forget+evict: $sha3"

say "write through the mount: queue, then upload"
echo "changed here" >"$W/mnt/notes.txt"; rc vfs/queue fs=nas: | head -20
sleep 7; echo "server has: $(cat "$W/src/notes.txt")"

say "publiclink on webdav (expect unsupported)"; rc operations/publiclink fs=nas: remote=notes.txt 2>&1 | tail -2

say "OAuth config states (dropbox, non-interactive)"
rc config/create name=dbx type=dropbox parameters='{}' opt='{"nonInteractive":true}' 2>&1 | head -30

say "core/bwlimit"; rc core/bwlimit rate=1M:2M

say "memory of rcd"; ps -o rss=,vsz= -p $RCD

say "unmount and quit"
rc mount/unmount mountPoint="$W/mnt"; rc core/quit; sleep 1; kill $DAV 2>/dev/null
kill -0 $RCD 2>/dev/null && echo "rcd still running" || echo "rcd gone"
tail -5 "$W/rcd.log"
