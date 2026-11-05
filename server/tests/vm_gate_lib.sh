# vm_gate_lib.sh — what Machines' release gates share (docs/VIRTUALIZATION.md 12): sourced by vm_integrity.sh,
# vm_recovery.sh and vm_leak.sh. Runs as a normal user who can open /dev/kvm, with qemu-img and passt installed.
#   BIN   folder with lord-vm          IMG   a Debian 13 "nocloud" qcow2 of this computer's architecture (copied)
#   W     work folder (made here, removed at the end unless KEEP=1)
set -u
VM="$BIN/lord-vm"
pass=0 fail=0 skip=0
ok()   { pass=$((pass + 1)); echo "PASS  $1"; }
bad()  { fail=$((fail + 1)); echo "FAIL  $1${2:+: $2}"; }
skp()  { skip=$((skip + 1)); echo "SKIP  $1${2:+: $2}"; }
t()    { if eval "$2"; then ok "$1"; else bad "$1" "${3:-}"; fi; }
now()  { date +%s; }
ms()   { date +%s%3N; }
field() { "$VM" --status "$1" --json 2>/dev/null | sed -n "s/.*\"$2\":\"\{0,1\}\([^,\"]*\).*/\1/p"; }
runpid() { sed -n 's/.*"pid":\([0-9]*\).*/\1/p' "$1/run.json" 2>/dev/null; }
qemupid() { sed -n 's/.*"qemu_pid":\([0-9]*\).*/\1/p' "$1/run.json" 2>/dev/null; }
wait_state() {   # DIR STATE SECONDS
    end=$(( $(now) + $3 ))
    while [ "$(now)" -lt "$end" ]; do [ "$(field "$1" state)" = "$2" ] && return 0; sleep 0.3; done
    return 1
}
wait_gone() {    # PID SECONDS
    end=$(( $(now) + $2 ))
    while [ "$(now)" -lt "$end" ]; do kill -0 "$1" 2>/dev/null || return 0; sleep 0.2; done
    return 1
}
wait_serial() {  # DIR TEXT SECONDS
    end=$(( $(now) + $3 ))
    while [ "$(now)" -lt "$end" ]; do grep -aq "$2" "$1/log/serial.log" 2>/dev/null && return 0; sleep 1; done
    return 1
}
# first-boot credentials: the console's questions answered, and a unit that keeps the disk busy (500 MB written and
# synced, over and over), so a kill lands in the middle of writes
gate_creds() {
    UNIT=$(printf '%s\n' '[Unit]' 'After=local-fs.target' '[Service]' \
        "ExecStart=/bin/sh -c 'echo \"<3>LORDVM-WRITER\" >/dev/kmsg; while :; do dd if=/dev/urandom of=/var/tmp/w bs=1M count=500 conv=fsync status=none; sync; done'")
    DROP=$(printf '[Unit]\nWants=lordvm-writer.service\n')
    umask 077
    PW="lvg-$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')"      # a throwaway root password: systemd-firstboot asks otherwise
    { echo "firstboot.locale=C.UTF-8"; echo "firstboot.keymap=us"; echo "firstboot.timezone=UTC"; echo "passwd.plaintext-password.root=$PW"
      echo "binary:systemd.extra-unit.lordvm-writer.service=$(printf '%s\n' "$UNIT" | base64 -w0)"
      echo "binary:systemd.unit-dropin.multi-user.target=$(printf '%s\n' "$DROP" | base64 -w0)"; } >"$W/creds"
    umask 022
}
start_busy() {   # DIR: started, and writing (the unit said so on the console)
    "$VM" --start "$1" --detach --keep-running --credentials-file "$W/creds" >/dev/null 2>&1 || return 1
    wait_state "$1" running 60 && wait_serial "$1" LORDVM-WRITER 120 && sleep 3
}
# qemu-img check of every disk of a machine (and its base): 0 clean, 3 leaked clusters only, anything else corrupt
check_disks() {  # DIR LABEL [leaks-ok]
    rc_all=0
    [ -f "$1/disks/system.qcow2" ] || { bad "$2" "no system disk in $1"; return; }
    for f in "$1"/disks/*.qcow2 $(sed -n 's/.*"base": "\(.*\)",*/\1/p' "$1/machine.json"); do
        [ -f "$f" ] || continue
        out=$(qemu-img check -U "$f" 2>&1); rc=$?
        if [ $rc = 0 ]; then :; elif [ $rc = 3 ] && [ "${3:-}" = leaks-ok ]; then echo "      $(basename "$f"): leaked clusters only (repaired at the next start)"
        else rc_all=1; echo "      $(basename "$f"): qemu-img check exit $rc: $(echo "$out" | tail -2 | tr '\n' ' ')"; fi
    done
    t "$2" '[ $rc_all = 0 ]'
}
# the next open: lord-vm starts it (recovery runs first), then it's turned off; the disks are checked again, clean
reopen() {       # DIR LABEL
    "$VM" --start "$1" --detach --keep-running >/dev/null 2>&1
    wait_state "$1" running 60 || true
    "$VM" --poweroff "$1" >/dev/null 2>&1
    wait_state "$1" stopped 30 || true
    check_disks "$1" "$2"
}
gate_cleanup() {
    for d in "$W"/*.lordvm; do [ -d "$d" ] && "$VM" --poweroff "$d" >/dev/null 2>&1; done
    [ "${KEEP:-}" = 1 ] || rm -rf "$W"
}
gate_begin() {
    [ "$(id -u)" = 0 ] && { echo "run as a normal user"; exit 2; }
    [ -x "$VM" ] || { echo "no lord-vm in $BIN"; exit 2; }
    [ -e "$W" ] && { echo "$W exists: give a new work folder"; exit 2; }
    mkdir -p "$W" || exit 2
    trap gate_cleanup EXIT
    trap 'exit 130' INT TERM
    echo "== $(uname -m), kernel $(uname -r), $(qemu-img --version | head -1), $(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
gate_end() {
    echo "== $1: $pass passed, $fail failed, $skip skipped"
    [ $fail = 0 ]
}
