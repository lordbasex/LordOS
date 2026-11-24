#!/bin/bash
# vm_sound_e2e.sh — a machine's speakers, microphone, screenshot and recording through a real lord-server (docs/
# VIRTUALIZATION.md 6.9, 6.20). Not in `make test`: it needs KVM, QEMU, a Linux guest image with qemu-guest-agent and an
# HDA driver (VM_SOUND_IMAGE, copied; alsa-utils is installed in the copy from the guest's own mirror when missing), this
# account's PipeWire (its pulse socket) and its default microphone, ffprobe and ffmpeg. The machine plays at volume 0: the
# test makes no sound in the room.
#
# The copy gets two guest services (prepared through lordvm-dev and qemu-guest-agent): a 440 Hz tone (speaker-test) and its
# microphone's level every 3 s (arecord, into /var/tmp/micrms.log with the boot's id). Then a headless lord-server, in its
# own user and mount namespaces with a scratch home and runtime folder (only the pulse socket of this account's PipeWire is
# mounted into it), runs tests/vm_sound_e2e.js as its theme: picture up, the guest plays and records, the microphone closed
# without LordOS's permission and open with it, a screenshot and an 8 s recording. Checked here: the speakers' stream is a
# PipeWire node named after the machine (pw-dump), the screenshot is a PNG of the guest's size, the recording has an H.264
# picture and an audio track with the tone in it (ffmpeg's volumedetect), and, read back from the guest at a second boot,
# its microphone heard silence without the permission and this computer's microphone with it.
cd "$(dirname "$0")/.." || exit 1
B=$(pwd)/build
for f in lord-vm lordvm-dev lord-vmdisplay lord-server; do [ -x "$B/$f" ] || { echo "skipped: needs build/$f"; exit 77; }; done
[ -f "${VM_SOUND_IMAGE:-}" ] || { echo "skipped: needs VM_SOUND_IMAGE"; exit 77; }
U=$(id -u); G=$(id -g); REALHOME=$(getent passwd "$U" | cut -d: -f6)
[ -r /dev/kvm ] && [ -S "/run/user/$U/pulse/native" ] && command -v pw-dump >/dev/null && command -v ffprobe >/dev/null && command -v unshare >/dev/null || { echo "skipped: needs KVM, this account's pulse socket, pw-dump, ffprobe and unshare"; exit 77; }
D=$(mktemp -d /var/tmp/lordos-sounde2e.XXXXXX) || exit 1
mkdir -p "$D/home/Machines" "$D/run" "$D/pulse" "$D/fake"
chmod 700 "$D/run" "$D/home"
M="$D/home/Machines/SndE2E.lordvm"
R=0
ok() { echo "ok: $*"; }
bad() { echo "FAIL: $*"; R=1; }
cleanup() { XDG_RUNTIME_DIR="$D/run" "$B/lord-vm" --poweroff "$M" >/dev/null 2>&1; [ -n "${SP:-}" ] && kill "$SP" 2>/dev/null; [ -n "$KEEP" ] || rm -rf "$D"; }
trap cleanup EXIT
"$B/lord-vm" --create "$M" --name SndE2E --arch amd64 --vcpus 2 --memory 2048 --display window --import "$VM_SOUND_IMAGE" >"$D/create.log" 2>&1 || { cat "$D/create.log"; exit 1; }
"$B/lord-vm" --set "$M" --agent on --transfer off --sound on --volume 0 --microphone on >/dev/null || exit 1

# ---- the guest's two services, through lordvm-dev ----
cat > "$D/prep.sh" <<'SH'
set -e
export DEBIAN_FRONTEND=noninteractive
command -v speaker-test >/dev/null || { apt-get -qq update; apt-get -qq install -y alsa-utils >/dev/null 2>&1; }
cat > /usr/local/bin/lordmic.sh <<'EOF'
#!/bin/sh
b=$(cat /proc/sys/kernel/random/boot_id)
while :; do
    arecord -q -d 2 -t raw -f S16_LE -r 48000 -c 1 | od -An -td2 -w2 -v | awk -v b="$b" '{ s += $1 * $1; n++ } END { if (n) printf "%s MICRMS %d %d\n", b, sqrt(s / n), n }' >> /var/tmp/micrms.log
    sleep 1
done
EOF
chmod 755 /usr/local/bin/lordmic.sh
printf '[Service]\nExecStart=/usr/bin/speaker-test -q -t sine -f 440 -c 2 -r 48000 -l 0\nRestart=always\n[Install]\nWantedBy=multi-user.target\n' > /etc/systemd/system/lordtone.service
printf '[Service]\nExecStart=/usr/local/bin/lordmic.sh\nRestart=always\n[Install]\nWantedBy=multi-user.target\n' > /etc/systemd/system/lordmic.service
amixer -q sset Master 80% unmute 2>/dev/null || true
amixer -q sset Capture 80% cap 2>/dev/null || true
alsactl store 2>/dev/null || true
systemctl daemon-reload
systemctl enable lordtone.service lordmic.service >/dev/null 2>&1
: > /var/tmp/micrms.log
echo prepared
SH
cat > "$D/drive.py" <<'PY'
import base64, json, re, subprocess, sys, threading, time
log, script, mode, argv = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[sys.argv.index('--') + 1:]
cmd = open(script).read() if mode == 'prep' else "awk '{ if (!($1 in o)) o[$1] = ++k; n[$1]++; if ($3 > 0) h[$1]++; if ($3 > m[$1]) m[$1] = $3 } END { for (b in n) print \"BOOT\", o[b], n[b], h[b] + 0, m[b] + 0 }' /var/tmp/micrms.log | sort -k2 -n"
q = json.dumps({'execute': 'guest-exec', 'arguments': {'path': '/bin/sh', 'arg': ['-c', 'echo ' + base64.b64encode(cmd.encode()).decode() + ' | base64 -d | sh 2>&1'], 'capture-output': True}}, separators=(',', ':'))
open(log + '.q', 'w').write(q)
p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
lines, lf = [], open(log, 'w')
def rd():
    for l in p.stdout:
        l = l.decode(errors='replace'); lines.append(l); lf.write(l); lf.flush()
threading.Thread(target=rd, daemon=True).start()
def say(c): lf.write('>> ' + c + '\n'); lf.flush(); p.stdin.write((c + '\n').encode()); p.stdin.flush()
def until(rx, t):
    end = time.time() + t
    while time.time() < end and not any(re.search(rx, l) for l in lines): time.sleep(0.2)
until('"state":"(paused|running)"', 60); say('resume'); time.sleep(30)
say('qmpf ' + log + '.q'); until('"text":"pid \\d+"', 10)
pid = [m.group(1) for l in lines for m in [re.search(r'"text":"pid (\d+)"', l)] if m][-1]
for _ in range(60):
    time.sleep(3); n = len(lines); say('qmp {"execute":"guest-exec-status","arguments":{"pid":%s}}' % pid)
    time.sleep(1)
    if any('"text":"exited' in l for l in lines[n:]): break
say('poweroff'); time.sleep(5); p.stdin.close()
try: p.wait(timeout=60)
except Exception: p.kill()
PY
XDG_RUNTIME_DIR="$D/run" LORDVM_DEV_STAY_PAUSED=1 timeout 400 python3 -I "$D/drive.py" "$D/prep.log" "$D/prep.sh" prep -- "$B/lordvm-dev" run "$M" --dev-qmp >/dev/null 2>&1
grep -q 'exitcode 0: prepared' "$D/prep.log" && ok "the guest's tone and microphone services are set up" || { bad "prepare: $(grep '"text":"exited' "$D/prep.log" | cut -c1-200)"; exit 1; }

# ---- lord-server ----
printf 'globalThis.SND_E2E = { dir: %s };\n' "\"$REALHOME/Machines/SndE2E.lordvm\"" > "$D/theme.js"
cat tests/vm_sound_e2e.js >> "$D/theme.js"
NS="unshare -Urm sh -c 'mount --bind /run/user/$U/pulse $D/pulse && mount -t tmpfs -o mode=700 tmpfs /run/user/$U && mkdir /run/user/$U/pulse && mount --bind $D/pulse /run/user/$U/pulse && mount --bind $D/home $REALHOME && exec unshare --user --map-user=$U --map-group=$G \"\$0\" \"\$@\"'"
eval "$NS" env -i PATH=/usr/bin:/bin HOME="$REALHOME" XDG_RUNTIME_DIR="/run/user/$U" LANG=C.UTF-8 LORD_GPU=0 "$B/lord-server" --headless 1024x768 --theme "$D/theme.js" --timeout 300 >"$D/server.log" 2>&1 &
SP=$!
L="$D/home/.cache/lordos/shell.log"
seen=0
for i in $(seq 1 300); do
    if [ $seen = 0 ] && grep -qs 'SNDE2E MARK speakers' "$L"; then
        seen=1
        pw-dump 2>/dev/null | python3 -I -c '
import json, sys
nodes = [o for o in json.load(sys.stdin) if o.get("type") == "PipeWire:Interface:Node"]
props = [o.get("info", {}).get("props", {}) for o in nodes]
hit = [p for p in props if p.get("media.class") == "Stream/Output/Audio" and p.get("application.name") == "Machines" and "SndE2E" in str(p.get("media.name", "")) + str(p.get("node.name", ""))]
print("found" if hit else "none: " + str([(p.get("application.name"), p.get("media.name")) for p in props if p.get("media.class", "").startswith("Stream")]))' > "$D/pw.txt"
        grep -q found "$D/pw.txt" && ok "the speakers are a PipeWire stream of Machines named after the machine" || bad "PipeWire: $(cat "$D/pw.txt")"
    fi
    grep -qs 'SNDE2E DONE\|SNDE2E FAIL exception' "$L" && break
    sleep 1
done
grep 'SNDE2E ok\|SNDE2E FAIL' "$L" | sed 's/^.*SNDE2E ok /ok: /; s/^.*SNDE2E FAIL /FAIL: /'
grep -q 'SNDE2E FAIL' "$L" && R=1
grep -q 'SNDE2E DONE' "$L" || { bad "the script didn't finish"; tail -20 "$L"; }
kill "$SP" 2>/dev/null; SP=
# the screenshot: a PNG of the guest's size
read -r _ _ SHOT SIZE <<< "$(grep -o 'SNDE2E SHOT .*' "$L" | head -1)"
SHOT=${SHOT/#$REALHOME/$D/home}
dim=$(python3 -I -c 'import struct,sys; b=open(sys.argv[1],"rb").read(24); print("%dx%d" % struct.unpack(">II", b[16:24]) if b[:8] == b"\x89PNG\r\n\x1a\n" else "not-png")' "$SHOT" 2>/dev/null)
[ -n "$SIZE" ] && [ "$dim" = "$SIZE" ] && ok "the screenshot is a PNG of the guest's picture ($dim)" || bad "screenshot: $dim vs $SIZE ($SHOT)"
# the recording: an H.264 picture and the guest's tone
MP4=$(ls "$D/home/Pictures/Screenshots/"*.mp4 2>/dev/null | head -1)
if [ -f "$MP4" ]; then
    st=$(ffprobe -v error -show_entries stream=codec_type,codec_name -of csv=p=0 "$MP4" | tr '\n' ' ')
    dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$MP4" | cut -d. -f1)
    echo "$st" | grep -q 'video' && echo "$st" | grep -q 'audio' && ok "the recording has a picture and sound ($st, ${dur}s)" || bad "recording streams: $st"
    [ "${dur:-0}" -ge 5 ] && ok "it is as long as it ran" || bad "recording length ${dur}s"
    mv=$(ffmpeg -nostats -i "$MP4" -vn -af volumedetect -f null - 2>&1 | sed -n 's/.*mean_volume: \(-*[0-9.]*\) dB.*/\1/p')
    awk -v v="$mv" 'BEGIN { exit !(v > -40) }' && ok "its sound is the guest's tone (mean volume $mv dB), recorded even with the speakers at 0" || bad "recording's sound: mean volume ${mv:-none} dB"
else bad "no recording in Pictures/Screenshots"; fi
# the microphone as the guest heard it: read back at another boot
XDG_RUNTIME_DIR="$D/run" LORDVM_DEV_STAY_PAUSED=1 timeout 200 python3 -I "$D/drive.py" "$D/read.log" /dev/null read -- "$B/lordvm-dev" run "$M" --dev-qmp >/dev/null 2>&1
# "BOOT <order> <recordings> <heard> <max RMS>" per boot: the one before this reading's is lord-server's
grep -o 'dev-out: .*' "$D/read.log" | sed 's/^dev-out: //' | tr '|' '\n' | grep '^BOOT ' > "$D/mic.txt"
read -r _ _ total heard max <<< "$(tail -2 "$D/mic.txt" | head -1)"
silent=$(( ${total:-0} - ${heard:-0} ))
[ "$silent" -ge 1 ] && ok "without the permission the guest's microphone heard silence ($silent recordings at 0)" || bad "no silent recording"
[ "$heard" -ge 1 ] && ok "with it the guest heard this computer's microphone ($heard recordings, RMS up to $max)" || bad "the guest never heard the microphone: $(head -c 400 "$D/mic.txt")"
[ $R = 0 ] && echo "vm_sound_e2e: all passed" || echo "vm_sound_e2e: FAILED (logs in $D with KEEP=1)"
exit $R
