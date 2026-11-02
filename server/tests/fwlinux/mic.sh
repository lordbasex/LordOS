#!/bin/bash
# mic.sh — "no microphone" for Linux apps inside PipeWire (src/sec/pwguard.c): the restricted
# app's capture streams are closed and the capture nodes hidden from it, live. Runs the real guard
# (build/pwguard-cli) against the session's PipeWire and records from inside an app scope with
# pw-record (native PipeWire) and parecord (PulseAudio API), and arecord (ALSA → PipeWire) if
# present. Needs an Audio/Source node (any, even a virtual sound card's).
#
#   bash server/tests/fwlinux/mic.sh
. "$(dirname "$0")/lib.sh"
D=$(cd "$(dirname "$0")" && pwd)
CLI=$D/../../build/pwguard-cli
[ -x "$CLI" ] || { echo "missing build/pwguard-cli (make -C server test)"; exit 2; }
export XDG_RUNTIME_DIR=/run/user/$U
[ -S "$XDG_RUNTIME_DIR/pipewire-0-manager" ] || { echo "skipped: needs the session's PipeWire"; exit 0; }
APP=desktop:fwmic
SC=$(scope fwmic)
OUTD=$(mktemp -d); chmod 755 "$OUTD"
GLOG=$OUTD/guard.log
restrict $APP ""
"$CLI" 120 > "$GLOG" 2>&1 &
GPID=$!
sleep 1
# rec <tool> <seconds>: records inside the scope; prints the bytes of audio it got
rec() {
    local f=$OUTD/$1.raw
    rm -f "$f"
    case $1 in
        pw-record) in_scope "$SC" timeout "$2" pw-record --rate 48000 --channels 1 --format s16 "$f" >/dev/null 2>&1 ;;
        parecord) in_scope "$SC" timeout "$2" parecord --raw --rate=48000 --channels=1 --format=s16le "$f" >/dev/null 2>&1 ;;
        arecord) in_scope "$SC" timeout "$2" arecord -q -D pipewire -f S16_LE -r 48000 -c 1 -t raw "$f" >/dev/null 2>&1 ;;
    esac
    stat -c %s "$f" 2>/dev/null || echo 0
}
TOOLS="pw-record parecord"
command -v arecord >/dev/null && [ -e /usr/share/alsa/alsa.conf.d/50-pipewire.conf ] && TOOLS="$TOOLS arecord"

echo "1. not restricted: each tool records"
for t in $TOOLS; do n=$(rec $t 3); [ "$n" -gt 20000 ] && ok "$t recorded $n bytes" || bad "$t recorded only $n bytes"; done

echo "2. no-mic: nothing is recorded"
restrict $APP no-mic
sleep 1.5                               # the guard rereads the rules every second
for t in $TOOLS; do n=$(rec $t 3); [ "$n" -lt 4096 ] && ok "$t got $n bytes" || bad "$t still recorded $n bytes"; done
grep -q "fwmic is restricted (no-mic)" "$GLOG" && ok "the guard closed the capture stream(s)" || bad "no closing in the guard's log"
seen=$(in_scope "$SC" pw-dump 2>/dev/null | grep -c '"media.class": "Audio/Source"')
all=$(pw-dump 2>/dev/null | grep -c '"media.class": "Audio/Source"')
[ "$seen" = 0 ] && [ "$all" -gt 0 ] && ok "the app sees no microphone node ($all exist)" || bad "the app sees $seen of $all microphone nodes"
python3 -c "import wave,sys; w=wave.open(sys.argv[1],'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000); w.writeframes(bytes(96000)); w.close()" "$OUTD/silence.wav"; chmod 644 "$OUTD/silence.wav"
in_scope "$SC" timeout 5 pw-play "$OUTD/silence.wav" >/dev/null 2>&1 && ok "playback still works for the app" || bad "playback failed"

echo "3. live: a recording that is running is cut when the restriction is set"
restrict $APP ""
sleep 1.5
f=$OUTD/live.raw
in_scope "$SC" timeout 8 pw-record --rate 48000 --channels 1 --format s16 "$f" >/dev/null 2>&1 &
RP=$!
sleep 2.5
before=$(stat -c %s "$f" 2>/dev/null || echo 0)
restrict $APP no-mic
sleep 3
mid=$(stat -c %s "$f" 2>/dev/null || echo 0)
sleep 2
after=$(stat -c %s "$f" 2>/dev/null || echo 0)
wait $RP 2>/dev/null
[ "$before" -gt 20000 ] && ok "it was recording ($before bytes)" || bad "it wasn't recording ($before)"
[ "$after" = "$mid" ] && ok "it stopped growing once restricted ($mid → $after)" || bad "it kept recording ($mid → $after)"

echo "4. lifted: records again"
restrict $APP ""
sleep 1.5
n=$(rec pw-record 3); [ "$n" -gt 20000 ] && ok "pw-record recorded $n bytes" || bad "pw-record recorded only $n bytes"

echo "5. outside the app's scope while it is restricted"
restrict $APP no-mic
sleep 1.5
f=$OUTD/out.raw; timeout 3 pw-record --rate 48000 --channels 1 --format s16 "$f" >/dev/null 2>&1
n=$(stat -c %s "$f" 2>/dev/null || echo 0)
[ "$n" -gt 20000 ] && ok "another program records ($n bytes)" || bad "another program got $n bytes"
echo "6. straight from the sound card (ALSA hw device, not PipeWire): refused by axia-netperm"
CAP=$(ls /dev/snd/pcmC*D*c 2>/dev/null | head -1)
if [ -n "$CAP" ] && [ -S /run/lordos/netperm.sock ]; then
    got=$(in_scope "$SC" python3 -c 'import os,sys,errno
try: os.close(os.open(sys.argv[1], os.O_RDONLY | os.O_NONBLOCK)); print("ok")
except OSError as e: print(errno.errorcode.get(e.errno))' "$CAP" 2>&1 | tail -1)
    [ "$got" = EPERM ] && ok "open $CAP → EPERM" || bad "open $CAP → $got"
    PB=$(ls /dev/snd/pcmC*D*p 2>/dev/null | head -1)
    got=$(in_scope "$SC" python3 -c 'import os,sys,errno
try: os.close(os.open(sys.argv[1], os.O_RDONLY | os.O_NONBLOCK)); print("ok")
except OSError as e: print(errno.errorcode.get(e.errno))' "$PB" 2>&1 | tail -1)
    [ "$got" != EPERM ] && ok "the playback node $PB is not refused ($got)" || bad "playback node refused"
    # an ALSA node the last scan hasn't seen (a microphone plugged in a moment ago): refused to the
    # app (fail closed), but not to the user's own services in user@ (the restriction union)
    NODE=$HOME/fwlinux-snd-new
    sudo rm -f "$NODE"; sudo mknod -m 666 "$NODE" c 116 250
    got=$(in_scope "$SC" python3 -c 'import os,sys,errno
try: os.close(os.open(sys.argv[1], os.O_RDONLY | os.O_NONBLOCK)); print("ok")
except OSError as e: print(errno.errorcode.get(e.errno))' "$NODE" 2>&1 | tail -1)
    [ "$got" = EPERM ] && ok "an unknown ALSA node → EPERM in the app's scope" || bad "unknown ALSA node in the scope → $got"
    if command -v systemd-run >/dev/null; then
        got=$(timeout 10 systemd-run --user --pipe --quiet python3 -c 'import os,sys,errno
try: os.close(os.open(sys.argv[1], os.O_RDONLY | os.O_NONBLOCK)); print("ok")
except OSError as e: print(errno.errorcode.get(e.errno))' "$NODE" 2>&1 | tail -1)
        [ "$got" != EPERM ] && ok "the same node from user@ is not refused ($got)" || bad "unknown ALSA node refused in user@"
    fi
    sudo rm -f "$NODE"
fi

echo "7. lying about who it is: still nothing recorded"
# an unrestricted recorder outside the scope, whose pid the restricted app claims
timeout 12 parecord --raw "$OUTD/decoy.raw" >/dev/null 2>&1 &
DECOY=$!
sleep 1
spoof() {   # spoof <name> <command...>: records in the scope with a lie; prints the bytes it got
    local name=$1; shift
    local f=$OUTD/$name.raw
    in_scope "$SC" env "$@" "$f" >/dev/null 2>&1
    stat -c %s "$f" 2>/dev/null || echo 0
}
n=$(spoof claim-pulse PULSE_PROP="application.process.id=$DECOY" timeout 3 parecord --raw)
[ "$n" -lt 4096 ] && ok "pulse client claiming another process's pid: $n bytes" || bad "spoofed application.process.id recorded $n bytes"
n=$(spoof fake-pulse PIPEWIRE_PROPS="{ \"pulse.server.type\": \"unix\", \"application.process.id\": \"$DECOY\" }" timeout 3 pw-record --rate 48000 --channels 1 --format s16)
[ "$n" -lt 4096 ] && ok "native client posing as a pulse client: $n bytes" || bad "fake pulse.server.type recorded $n bytes"
n=$(spoof bad-pid PULSE_PROP="application.process.id=999999" timeout 3 parecord --raw)
[ "$n" -lt 4096 ] && ok "pulse client claiming a pid that doesn't exist: $n bytes" || bad "unresolvable pid recorded $n bytes"
kill $DECOY 2>/dev/null; wait $DECOY 2>/dev/null
# a recorder in another PID namespace reports its inner pid, so its outer pid has a connection no
# client claims: claiming that pid must not pass
if unshare --user --pid --fork --map-current-user true 2>/dev/null; then
    unshare --user --pid --fork --map-current-user timeout 12 parecord --raw "$OUTD/nsdecoy.raw" >/dev/null 2>&1 &
    sleep 1.5
    NSDECOY=$(pgrep -u "$U" -n -x parecord)
    n=$(spoof claim-nspid PULSE_PROP="application.process.id=$NSDECOY" timeout 3 parecord --raw)
    [ "$n" -lt 4096 ] && ok "claiming the outer pid of a process in another PID namespace: $n bytes" || bad "claimed outer pid recorded $n bytes"
    pkill -u "$U" -x parecord 2>/dev/null; sleep 0.5
fi

echo "8. loading a module through pipewire-pulse (a loopback from the microphone): its stream is closed"
SRC=$(pactl get-default-source 2>/dev/null)
NULLMOD=$(in_scope "$SC" pactl load-module module-null-sink sink_name=fwmicnull 2>/dev/null)
LOOPMOD=$(in_scope "$SC" pactl load-module module-loopback source="$SRC" sink=fwmicnull 2>/dev/null)
sleep 2
if grep -q "pipewire-pulse (a loaded module) is restricted (no-mic)" "$GLOG"; then ok "the loopback's capture stream was closed"
elif [ -z "$LOOPMOD" ]; then ok "the loopback could not start"
else bad "a module's capture stream stayed open"; fi
[ -n "$LOOPMOD" ] && pactl unload-module "$LOOPMOD" 2>/dev/null
[ -n "$NULLMOD" ] && pactl unload-module "$NULLMOD" 2>/dev/null

echo "9. escaped to the user manager while an app is restricted, posing as the sound server's unit"
UM=/sys/fs/cgroup/user.slice/user-$U.slice/user@$U.service/app.slice
if command -v systemd-run >/dev/null && [ -d "$UM" ]; then
    f=$OUTD/fakeunit.raw
    for leaf in "fwfake.scope/session.slice/pipewire.service" "fwfake.scope/session.slice/wireplumber.service"; do
        rm -f "$f"
        timeout 10 systemd-run --user --scope -q bash -c 'mkdir -p "$1/$2" && echo $$ > "$1/$2/cgroup.procs" && exec timeout 3 pw-record --rate 48000 --channels 1 --format s16 "$3"' _ "$UM" "$leaf" "$f" >/dev/null 2>&1
        n=$(stat -c %s "$f" 2>/dev/null || echo 0)
        [ "$n" -lt 4096 ] && ok "a recorder in user@…/$leaf: $n bytes" || bad "a nested $leaf recorded $n bytes"
    done
    for d in session.slice/pipewire.service session.slice/wireplumber.service session.slice ""; do rmdir "$UM/fwfake.scope/$d" 2>/dev/null; done
    rm -f "$f"
    f=$OUTD/um.raw
    timeout 10 systemd-run --user --scope -q timeout 3 pw-record --rate 48000 --channels 1 --format s16 "$f" >/dev/null 2>&1
    n=$(stat -c %s "$f" 2>/dev/null || echo 0)
    [ "$n" -lt 4096 ] && ok "systemd-run --user pw-record: $n bytes" || bad "systemd-run --user recorded $n bytes"
fi
restrict $APP ""
sleep 1.5
f=$OUTD/after.raw; timeout 3 pw-record --rate 48000 --channels 1 --format s16 "$f" >/dev/null 2>&1
n=$(stat -c %s "$f" 2>/dev/null || echo 0)
all=$(pw-dump 2>/dev/null | grep -c '"media.class": "Audio/Source"')
[ "$n" -gt 20000 ] && [ "$all" -gt 0 ] && ok "once lifted, the desktop still records ($n bytes, $all microphone nodes)" || bad "the desktop lost the microphone ($n bytes, $all nodes)"
kill $GPID 2>/dev/null; wait $GPID 2>/dev/null
rm -rf "$OUTD"
fwlinux_done
