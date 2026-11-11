#!/bin/sh
# gpu_cycle.sh — a session leaves the monitor and comes back N times while Wayland apps keep
# drawing; checks that their windows are not black afterwards and that fds and memory stay flat
# (docs/GPU.md, "When the GPU context is re-created"). Integration test, not part of `make test`:
# it needs a free DRM card (stop lordos.service first), sudo (the seat side is root, as
# lord-sessiond is) and, for the dmabuf client, /dev/udmabuf.
#
#   sh tests/gpu_cycle.sh <lord-server> <cycles> [card] [gpu]     (gpu: 1 = LORD_GPU=1, 0 = CPU)
#
# Prints per-cycle fd count and RSS of the session and leaves the DRM snapshots in $OUT
# (default /tmp/gpu-cycle). Run from server/ after `make build/dmabuf-client build/seat-cycle`.
set -eu
BIN=$1
CYCLES=${2:-10}
CARD=${3:-/dev/dri/card0}
GPU=${4:-1}
OUT=${OUT:-/tmp/gpu-cycle}
LIB=${LIB:-/usr/lib/lordos}
RT=$OUT/rt
rm -rf "$OUT"
mkdir -p "$RT" "$OUT/home"
chmod 700 "$RT"

env -i PATH=/usr/bin:/bin HOME="$OUT/home" XDG_RUNTIME_DIR="$RT" LORD_GPU="$GPU" LORD_GPU_DEBUG=1 \
    "$BIN" --headless 1280x800 --theme "$LIB/themes/default/theme.bundle.js" \
    --fonts "$LIB/assets/fonts" --shell "$LIB/assets/shell" 2>"$OUT/stderr.log" &
SESS=$!
trap 'kill $SESS 2>/dev/null; sudo pkill -f "$OUT/rt" 2>/dev/null || true' EXIT
for _ in $(seq 50); do [ -S "$RT/lordos/remote.sock" ] && break; sleep 0.2; done
sleep 3
WL=$(cd "$RT" && ls wayland-* | grep -v lock | head -1)

# a dmabuf client (udmabuf, linear) and, if there is one, Chromium on Wayland with an animation
sudo env XDG_RUNTIME_DIR="$RT" WAYLAND_DISPLAY="$WL" build/dmabuf-client --frames 100000000 --size 480x320 \
    >"$OUT/dmabuf-client.log" 2>&1 &
if command -v chromium >/dev/null; then
    cat >"$OUT/anim.html" <<'EOF'
<body style="margin:0;background:#123"><canvas id=c width=600 height=400></canvas><script>
const g=c.getContext('2d');let t=0;(function f(){t++;g.fillStyle=`hsl(${t%360},80%,50%)`;g.fillRect(0,0,600,400);
g.fillStyle='#fff';g.font='48px sans-serif';g.fillText('frame '+t,40,200);requestAnimationFrame(f)})()</script>
EOF
    env -i PATH=/usr/bin:/bin HOME="$OUT/home" XDG_RUNTIME_DIR="$RT" WAYLAND_DISPLAY="$WL" \
        chromium --ozone-platform=wayland --user-data-dir="$OUT/chrome" --no-first-run --window-size=700,500 \
        "file://$OUT/anim.html" >"$OUT/chromium.log" 2>&1 &
fi
sleep 8

echo "before: fds=$(ls /proc/$SESS/fd | wc -l) rss_kb=$(awk '/VmRSS/{print $2}' /proc/$SESS/status)"
# after each cycle (the session off the monitor): open fds and RSS of the session
sudo build/seat-cycle "$RT/lordos/remote.sock" "$CARD" "$CYCLES" 3000 1500 "$OUT/c" \
    "echo cycle \$CYCLE: fds=\$(ls /proc/$SESS/fd | wc -l) rss_kb=\$(awk '/VmRSS/{print \$2}' /proc/$SESS/status)"
echo "end (on the monitor): fds=$(ls /proc/$SESS/fd | wc -l) rss_kb=$(awk '/VmRSS/{print $2}' /proc/$SESS/status)"
grep -oE " (gpu|monitor|wayland): .*" "$OUT/home/.cache/lordos/compositor.log" | sed "s/^ //" | sort | uniq -c | sort -rn | head -20
echo "snapshots: $OUT/c-NN.bmp (each time it was shown) and $OUT/c-final.bmp"
