#!/bin/sh
# lock_e2e.sh — the lock after the display turns off, end to end (issue #36), on a VM with sudo.
#
#   tests/lock_e2e.sh idle|sleep <outdir> [server dir]
#
# A session lord-server and a login screen (--headless, as lord-sessiond starts them, LORDOS_SESSIOND=1)
# with "Require password after the display turns off: Immediately"; the monitor is a vkms card that
# build/lock-seat (tests/lock_seat.c, run as root) lends the way lord-sessiond does; the system bus is a
# private dbus-daemon where tests/fake_logind.py plays logind's sleep.
#   idle   the display turns off after 8 s without input: the session must ask to lock (dark) at once
#   sleep  display off after 10 min, so only the sleep can lock: logind's PrepareForSleep(true) at 8 s
#          (the lid closed or the power button pressed) must lock before the computer sleeps, and the
#          sleep must wait (the delay inhibitor) until the monitor was taken
# LOCK_DELAY=-1 (Never) or a delay longer than the test is the control: no lock may come (exit 3).
# Pictures: <outdir>/<mode>-1-desktop.bmp, -2-lock.bmp (and -3-lock-woken.bmp after an idle lock);
# the steps in <outdir>/<mode>.log. Needs make build/lord-server build/lock-seat and the JS bundles.
set -u
MODE=${1:?idle or sleep}
OUT=${2:?outdir}
SRV=$(cd "${3:-$(dirname "$0")/..}" && pwd)
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
T=$(mktemp -d /tmp/lock-e2e.XXXXXX)
LOG="$OUT/$MODE.log"
: > "$LOG"
mkdir -p "$T/rt" "$T/grt" "$T/home/.config/lordos" "$T/ghome"
chmod 700 "$T/rt" "$T/grt"
if [ "$MODE" = idle ]; then OFF=8; SLEEP_AT=999; else OFF=600; SLEEP_AT=8; fi
# LOCK_DELAY: "Require password after the display turns off" in seconds (0 Immediately, -1 Never)
printf '{"power":{"offAC":%d,"offBattery":%d,"lockDelay":%d,"sleepAC":0,"sleepBattery":0}}\n' "$OFF" "$OFF" "${LOCK_DELAY:-0}" > "$T/home/.config/lordos/settings.json"

cleanup() {
    for p in ${PIDS:-}; do kill "$p" 2>/dev/null; done
    sleep 1
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

# the monitor: a vkms card nobody else uses; without vkms (Debian's kernel has none) no card at all, the
# frames in memory standing for the screen (LORD_IDLE_HEADLESS=1, development only)
CARD=-
HEADLESS=
if sudo modprobe vkms 2>/dev/null; then
    for c in /sys/class/drm/card[0-9]; do
        [ "$(basename "$(readlink -f "$c/device/driver")")" = vkms ] && CARD=/dev/dri/$(basename "$c")
    done
fi
[ "$CARD" = - ] && HEADLESS=LORD_IDLE_HEADLESS=1
echo "monitor: $CARD ${HEADLESS:-(vkms)}" >> "$LOG"

# the system bus and logind
dbus-daemon --session --address="unix:path=$T/bus" --nofork --nopidfile > /dev/null 2>&1 &
PIDS="$!"
sleep 0.5
python3 -I "$SRV/tests/fake_logind.py" "unix:path=$T/bus" "$SLEEP_AT" 4 "$LOG" &
PIDS="$PIDS $!"
sleep 0.5

run() {   # run <runtime dir> <home> <log> <args...>
    rt=$1; home=$2; slog=$3; shift 3
    env -i PATH=/usr/bin:/bin HOME="$home" XDG_RUNTIME_DIR="$rt" LANG=en_US.UTF-8 LORDOS_SESSIOND=1 LORD_GPU=0 $HEADLESS \
        DBUS_SYSTEM_BUS_ADDRESS="unix:path=$T/bus" \
        "$SRV/build/lord-server" --headless 1280x800 --fonts "$SRV/../assets/fonts" --shell "$SRV/../assets/shell" --timeout 90 "$@" > "$slog" 2>&1 &
    PIDS="$PIDS $!"
}
run "$T/rt" "$T/home" "$OUT/$MODE-session.log" --theme "$SRV/../themes/default/theme.bundle.js"
run "$T/grt" "$T/ghome" "$OUT/$MODE-greeter.log" --greeter --theme "$SRV/../themes/greeter/theme.bundle.js"
for i in $(seq 100); do
    [ -S "$T/rt/lordos/remote.sock" ] && [ -S "$T/grt/lordos/remote.sock" ] && break
    sleep 0.2
done
sleep 3     # the themes start

sudo "$SRV/build/lock-seat" "$T/rt/lordos/remote.sock" "$T/grt/lordos/remote.sock" "$CARD" "$OUT/$MODE" 20000 500 >> "$LOG" 2>&1
RC=$?
sleep 5     # logind's wake
# lord-server writes its log to ~/.cache/lordos when its output is not a terminal
cp "$T/home/.cache/lordos/compositor.log" "$OUT/$MODE-session.log" 2>/dev/null
cp "$T/ghome/.cache/lordos/compositor.log" "$OUT/$MODE-greeter.log" 2>/dev/null
grep -h -E " (sleep|monitor): |sysbus" "$OUT/$MODE-session.log" | sed 's/^/session: /' >> "$LOG"
cat "$LOG"
for p in $PIDS; do kill "$p" 2>/dev/null; done
sleep 1
sudo chown "$(id -u):$(id -g)" "$OUT"/*.bmp 2>/dev/null
[ "$CARD" != - ] && sudo modprobe -r vkms 2>/dev/null
echo "lock-seat exit: $RC"
exit $RC
