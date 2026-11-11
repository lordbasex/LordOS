#!/bin/sh
# keylag.sh — where does the typing delay come from? (docs/TROUBLESHOOTING.md, "Typing feels slow")
#
# Run it as the logged-in user, in the LordOS Terminal (keys typed there are never traced):
#
#     sh keylag.sh [seconds]          (default 30)
#
# 1. It prints what matters for input latency: version, GPU or CPU composition, virtual machine,
#    guest tools, the boot flags, and whether Chromium has complained about the compositor.
# 2. If the session was started with the key latency trace (boot menu: add lordos.inputdebug=1 to
#    the kernel line, LordOS 0.8.1 or later), it asks you to type in the slow app for a few
#    seconds and prints, per stage, p50/p95/max in ms after the kernel's evdev timestamp:
#    lord-server read it (rx), the app got it (flush), the app answered (commit) and the frame with
#    it was on screen (shown). The trace has timings only: no key, character or modifier.
# 3. It says which stage is slow and what to try.
set -u
export LC_ALL=C          # decimal points in awk and printf
SECS=${1:-30}
RT=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
TRACE=$RT/lordos-keylag.log
# the session's logs (server/src/lordlog.h): /var/log/lordos/users/<user>, or ~/.cache/lordos without lord-sessiond
LOGDIR=/var/log/lordos/users/$(id -un)
[ -d "$LOGDIR" ] && [ ! -L "$LOGDIR" ] || LOGDIR=$HOME/.cache/lordos
SLOG=$LOGDIR/compositor.log
[ -r "$SLOG" ] || SLOG=$LOGDIR/session.log            # an older version's
ALOG=$LOGDIR/apps

say() { printf '%s\n' "$*"; }

say "== LordOS key latency check"
ver=$(dpkg-query -W -f '${Version}' lordos 2>/dev/null || echo "?")
virt=$(systemd-detect-virt 2>/dev/null || echo "?")
cmd=$(cat /proc/cmdline)
comp="unknown"
if [ -r "$SLOG" ]; then
    if grep -q "gpu: GPU composition" "$SLOG"; then comp="GPU ($(grep -m1 'gpu: GPU composition' "$SLOG" | sed 's/.*composition (\([^,]*\).*/\1/'))"
    elif grep -q "gpu:" "$SLOG"; then comp="CPU"; fi
fi
tools="not installed"
if command -v vmtoolsd >/dev/null 2>&1; then
    tools="installed, $(systemctl is-active open-vm-tools 2>/dev/null)"
    ts=$(vmware-toolbox-cmd timesync status 2>/dev/null) && tools="$tools, host time sync $ts"
fi
say "version:        $ver"
say "virtualization: $virt"
say "composition:    $comp"
say "guest tools:    $tools"
case " $cmd " in *" lordos.gpu=0 "*) say "boot flag:      lordos.gpu=0 (CPU composition forced)";; esac
case " $cmd " in *" lordos.inputdebug=1 "*) say "boot flag:      lordos.inputdebug=1 (key latency trace)";; esac
buggy=0
[ -d "$ALOG" ] && buggy=$(cat "$ALOG"/*.log 2>/dev/null | grep -c "buggy presentation feedback")
say "Chromium:       \"buggy presentation feedback\" $buggy time(s) in this session's app log"
say ""

if ! grep -q "keylag: key latency trace on" "$SLOG" 2>/dev/null; then
    say "No key latency trace in this session."
    if [ "$buggy" -gt 0 ] && [ "${comp#GPU}" != "$comp" ]; then
        say "-> Chromium's warning on the GPU path is the presentation bug fixed in LordOS 0.8.1: a Linux"
        say "   app's new frame was only shown when something else on the screen changed (up to ~1 s late)."
        say "   To confirm on this build in 2 minutes: reboot, press 'e' in the boot menu, add lordos.gpu=0"
        say "   at the end of the 'linux' line, Ctrl+X, then type in Chromium. If the delay is gone, that was it."
    fi
    say "With LordOS 0.8.1 or later: add lordos.inputdebug=1 the same way (one boot only) and run this again."
    exit 0
fi

before=0
[ -f "$TRACE" ] && before=$(wc -l < "$TRACE")
say "Click the slow app (Chromium, Firefox...) and type normally for $SECS seconds, starting now."
say "(The trace stops by itself 120 s after the first traced key; log out and in for a new one.)"
i=$SECS
while [ "$i" -gt 0 ]; do printf '\r%3d s ' "$i"; sleep 1; i=$((i - 1)); done
printf '\r       \n'
if [ ! -f "$TRACE" ]; then
    say "Nothing was traced: no key reached a Linux app (keys for LordOS's own apps are never traced)."
    exit 1
fi
tail -n +"$((before + 1))" "$TRACE" | grep '^keylag n=' | grep ' press ' > "$RT/keylag-run.txt"
n=$(wc -l < "$RT/keylag-run.txt")
grep -q '^keylag stop' "$TRACE" && say "note: $(grep -m1 '^keylag stop' "$TRACE")"
if [ "$n" -lt 5 ]; then say "Only $n key presses traced: type a bit more and run it again."; exit 1; fi

# p50 / p95 / max of one field (rx=, flush=...), values of -1 left out
stats() {
    sed -n "s/.* $1=\([-0-9.]*\).*/\1/p" "$RT/keylag-run.txt" | awk '$1 >= 0' | sort -n | awk '
        { v[NR] = $1 }
        END { if (!NR) { print "  -      -      -"; exit }
              i50 = int((NR - 1) * 0.50) + 1; i95 = int((NR - 1) * 0.95) + 1
              printf "%7.1f %7.1f %7.1f\n", v[i50], v[i95], v[NR] }'
}
say ""
say "$n key presses (ms after the kernel event)    p50     p95     max"
for f in rx flush commit shown; do
    case $f in
        rx) label="lord-server read the key        (rx)";;
        flush) label="the app received it         (flush)";;
        commit) label="the app drew its answer    (commit)";;
        shown) label="that frame was on screen    (shown)";;
    esac
    printf '%-44s %s\n' "$label" "$(stats $f)"
done
p95() { stats "$1" | awk '{print int($2)}'; }
rx=$(p95 rx); fl=$(p95 flush); co=$(p95 commit); sh=$(p95 shown)
# keys arriving in clumps (< 15 ms apart): they were held back before the guest's kernel
clump=$(sed -n 's/.* ev=\([0-9.]*\) .*/\1/p' "$RT/keylag-run.txt" | awk 'NR > 1 && $1 - p < 15 { c++ } { p = $1 } END { printf "%d", 100 * c / (NR > 1 ? NR - 1 : 1) }')
say ""
say "key presses arriving less than 15 ms after the previous one: $clump %"
say ""
if [ "$rx" -gt 30 ]; then
    say "-> lord-server is slow to read keys (busy main loop). Note what is open and report it with this output."
elif [ "$((co - fl))" -gt 50 ]; then
    say "-> The app itself is slow to answer (commit - flush). Try it with fewer tabs or without the video."
elif [ "$((sh - co))" -gt 50 ]; then
    say "-> Frames reach the screen late (shown - commit): compositor presentation. Report it with this output;"
    say "   meanwhile boot with lordos.gpu=0 (CPU composition)."
elif [ "$sh" -le 40 ] && [ "$clump" -ge 20 ]; then
    say "-> Inside the guest every stage is fast, but keys arrive in clumps: the delay is before the guest"
    say "   kernel, in the hypervisor. On VMware try: 'Accelerate 3D graphics' off, fewer vCPUs, or in the"
    say "   .vmx keyboard.vusb.enable = \"TRUE\" (docs/TROUBLESHOOTING.md)."
elif [ "$sh" -le 40 ]; then
    say "-> Every stage inside the guest is fast (p95 under 40 ms). If typing still feels slow, the delay is"
    say "   before the guest kernel (the hypervisor or the host): see docs/TROUBLESHOOTING.md."
else
    say "-> Mixed result: keep this output and report it."
fi
rm -f "$RT/keylag-run.txt"
