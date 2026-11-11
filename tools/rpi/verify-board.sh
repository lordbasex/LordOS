#!/bin/sh
# verify-board.sh — checks a Raspberry Pi freshly flashed with the LordOS image and prints a
# PASS/FAIL table. See docs/RASPBERRY.md, "After flashing".
#
# Usage, on the board (as root, or with sudo, so the kernel log and listening sockets are visible):
#   sudo sh verify-board.sh                    # system checks only (nothing to install)
#   sudo sh verify-board.sh --repo ~/LordOS    # also builds the tree and runs make -C server test-full
#                                              # (needs the build packages of "Developing on the board")
#
# It changes nothing on the system: it only reads /sys, /proc, the kernel log and the configs, and
# with --repo builds inside that tree as the user who owns it. Exit status: 0 if nothing failed.
set -u

REPO=""
while [ $# -gt 0 ]; do
    case "$1" in
        --repo) REPO=${2:-}; shift 2 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "verify-board.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

PASS=0; FAIL=0; WARN=0
ROWS=""
row() {   # status, check, detail
    case "$1" in PASS) PASS=$((PASS + 1)) ;; FAIL) FAIL=$((FAIL + 1)) ;; *) WARN=$((WARN + 1)) ;; esac
    ROWS="$ROWS$(printf '%-5s | %-26s | %s' "$1" "$2" "$3")
"
}
[ "$(id -u)" = 0 ] || echo "verify-board.sh: not root: some checks (kernel log, sockets) may show WARN" >&2

# ---- board and kernel ----
MODEL=$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo unknown)
KREL=$(uname -r)
row INFO "board" "$MODEL"
case "$KREL" in
    *-lordos-rpi-*|*-lordos.*-rpi-*) row PASS "kernel (LordOS build)" "$KREL" ;;
    *) row FAIL "kernel (LordOS build)" "$KREL is not a -lordos[.<rev>]-rpi-* kernel (Raspberry Pi's has no Landlock)" ;;
esac
PS=$(getconf PAGESIZE 2>/dev/null || echo ?)
case "$KREL:$PS" in
    *-rpi-2712:16384|*-rpi-v8:4096) row PASS "page size" "$PS bytes" ;;
    *) row WARN "page size" "$PS bytes for $KREL" ;;
esac

# ---- Landlock and the rest of the isolation ----
LSM=$(cat /sys/kernel/security/lsm 2>/dev/null || echo "")
case ",$LSM," in
    *,landlock,*) row PASS "Landlock in the LSM list" "$LSM" ;;
    *) row FAIL "Landlock in the LSM list" "/sys/kernel/security/lsm = '${LSM:-unreadable}'" ;;
esac
# the bpf LSM and BTF: lord-netperm's "No Bluetooth" program for Linux apps (docs/FIREWALL.md)
case ",$LSM," in
    *,bpf,*) row PASS "BPF LSM in the LSM list" "$LSM" ;;
    *) row FAIL "BPF LSM in the LSM list" "no bpf in '${LSM:-unreadable}' (\"No Bluetooth\" not enforced)" ;;
esac
if [ -s /sys/kernel/btf/vmlinux ]; then row PASS "kernel BTF" "/sys/kernel/btf/vmlinux, $(($(stat -c %s /sys/kernel/btf/vmlinux) / 1024)) KiB"
else row FAIL "kernel BTF" "no /sys/kernel/btf/vmlinux (lord-netperm can't attach its LSM program)"; fi
if systemctl is-active -q lord-netperm.service 2>/dev/null; then row PASS "lord-netperm" "active"
elif systemctl cat lord-netperm.service >/dev/null 2>&1; then row FAIL "lord-netperm" "$(systemctl is-active lord-netperm.service)"; fi
KLOG=$( (dmesg 2>/dev/null || journalctl -k -b --no-pager 2>/dev/null) | grep -i 'landlock:' | head -1)
if echo "$KLOG" | grep -qi 'up and running'; then row PASS "Landlock initialized" "$(echo "$KLOG" | sed 's/^.*\] //')"
elif [ -z "$KLOG" ]; then row WARN "Landlock initialized" "no landlock line in the kernel log$([ "$(id -u)" = 0 ] || echo " (run as root)")"
else row FAIL "Landlock initialized" "$KLOG"; fi
CFG=/boot/config-$KREL
if [ -f "$CFG" ]; then
    MISS=""
    for o in SECURITY_LANDLOCK SECCOMP_FILTER CGROUP_BPF BPF_SYSCALL BPF_LSM DEBUG_INFO_BTF MEMCG; do
        grep -q "^CONFIG_$o=y" "$CFG" || MISS="$MISS $o"
    done
    if [ -z "$MISS" ]; then row PASS "kernel config" "LANDLOCK SECCOMP_FILTER CGROUP_BPF BPF_SYSCALL BPF_LSM DEBUG_INFO_BTF MEMCG"
    else row FAIL "kernel config" "missing:$MISS"; fi
    # sock_diag SOCK_DESTROY (0.8.4's kernel, 6.18.57-3+lordos): lord-netperm closes the open
    # connections a new rule denies (docs/FIREWALL.md, "Existing connections are cut"); without it
    # they are only dropped and the app stalls until its own timeout
    if grep -q '^CONFIG_INET_DIAG_DESTROY=y' "$CFG" && [ "$(grep -Ec '^CONFIG_INET_(TCP|UDP)_DIAG=[ym]' "$CFG")" = 2 ]; then
        row PASS "socket destroy" "INET_DIAG_DESTROY, INET_TCP_DIAG, INET_UDP_DIAG"
    else row FAIL "socket destroy" "no INET_DIAG_DESTROY/INET_UDP_DIAG: blocked connections stall instead of closing"; fi
fi
grep -q '^Seccomp:' /proc/self/status && row PASS "seccomp" "available" || row FAIL "seccomp" "no Seccomp field in /proc/self/status"
CTRL=$(cat /sys/fs/cgroup/cgroup.controllers 2>/dev/null || echo "")
case " $CTRL " in
    *" memory "*) row PASS "cgroup v2 memory" "controllers: $CTRL" ;;
    *) row FAIL "cgroup v2 memory" "controllers: '${CTRL}' (cmdline.txt needs cgroup_enable=memory)" ;;
esac

# ---- first boot ----
DF=$(df -h / | awk 'NR == 2 { print $2 " (" $5 " used)" }')
if [ -e /var/lib/lordos/rpi/resize-pending ]; then row FAIL "root partition grown" "resize still pending: $DF"
else row PASS "root partition grown" "$DF"; fi
LEFT=""
for f in ssh ssh.txt remote-desktop remote-desktop.txt lordos-user.txt userconf.txt; do
    [ -e "/boot/firmware/$f" ] && LEFT="$LEFT $f"
done
if [ -z "$LEFT" ]; then row PASS "switch files consumed" "none left on /boot/firmware"
else row WARN "switch files consumed" "still there:$LEFT (remote-desktop waits for an account with a password)"; fi
if [ -e /var/lib/lordos/rpi/setup-mode ]; then row WARN "user account" "setup mode (create your account in Settings → Users)"
else row PASS "user account" "first-boot user created (login screen)"; fi

# ---- display and GPU ----
CONN=""
for s in /sys/class/drm/card*-*/status; do
    [ -f "$s" ] || continue
    c=$(basename "$(dirname "$s")")
    st=$(cat "$s")
    m=$(head -1 "$(dirname "$s")/modes" 2>/dev/null)
    CONN="$CONN ${c#card?-}=$st${m:+@$m}"
done
if echo "$CONN" | grep -q '=connected'; then row PASS "KMS outputs" "${CONN# }"
elif [ -n "$CONN" ]; then row WARN "KMS outputs" "nothing connected:${CONN}"
else row FAIL "KMS outputs" "no DRM connectors (vc4 not loaded? dtoverlay=vc4-kms-v3d)"; fi
DRV=""
for d in /sys/class/drm/card?; do
    [ -e "$d/device/driver" ] && DRV="$DRV $(basename "$d")=$(basename "$(readlink "$d/device/driver")")"
done
row INFO "DRM cards" "${DRV# }"
if command -v eglinfo >/dev/null; then
    R=$(eglinfo -B -p gbm 2>/dev/null | grep -m1 -i 'renderer' | sed 's/^[^:]*: *//')
    if echo "$R" | grep -q 'V3D'; then row PASS "GPU (EGL on GBM)" "$R"
    else row FAIL "GPU (EGL on GBM)" "renderer: '${R:-none}'"; fi
else row WARN "GPU (EGL on GBM)" "eglinfo missing (mesa-utils)"; fi
SLOG=$(ls -t /var/log/lordos/users/*/compositor.log /home/*/.cache/lordos/compositor.log /home/*/.cache/lordos/session.log 2>/dev/null | grep -v lordos-greeter | head -1)
if [ -n "$SLOG" ]; then
    G=$(grep -m1 'gpu:' "$SLOG" | sed 's/^.*gpu: //')
    if echo "$G" | grep -q 'GPU composition'; then row PASS "LordOS GPU composition" "$G"
    else row WARN "LordOS GPU composition" "${G:-no gpu: line} ($SLOG)"; fi
fi

# ---- fan and temperature ----
FAN=""
for h in /sys/class/hwmon/hwmon*; do
    n=$(cat "$h/name" 2>/dev/null)
    case "$n" in *fan*) FAN="$n"; [ -f "$h/fan1_input" ] && FAN="$FAN $(cat "$h/fan1_input") rpm"
                        [ -f "$h/pwm1" ] && FAN="$FAN, pwm $(cat "$h/pwm1")/255" ;; esac
done
if [ -n "$FAN" ]; then row PASS "fan detected" "$FAN"
else row WARN "fan detected" "no fan hwmon (no Active Cooler or case fan connected?)"; fi
if systemctl list-unit-files lord-fand.service >/dev/null 2>&1 && systemctl cat lord-fand.service >/dev/null 2>&1; then
    if systemctl is-active -q lord-fand.service; then row PASS "lord-fand" "active"
    else row FAIL "lord-fand" "$(systemctl is-active lord-fand.service)"; fi
fi
T=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 0)
TC=$((T / 1000))
if [ "$TC" -gt 0 ] && [ "$TC" -lt 75 ]; then row PASS "SoC temperature" "${TC} °C"
elif [ "$TC" -ge 75 ]; then row FAIL "SoC temperature" "${TC} °C (≥ 75 °C idle: check the cooling)"
else row WARN "SoC temperature" "unreadable"; fi
if command -v vcgencmd >/dev/null; then
    TH=$(vcgencmd get_throttled 2>/dev/null | sed 's/^throttled=//')
    if [ "$TH" = 0x0 ]; then row PASS "throttling" "throttled=0x0"
    else row WARN "throttling" "throttled=$TH (under-voltage or heat; 0x50000 = happened before)"; fi
fi

# ---- Bluetooth: the on-board CYW43455 on its UART (docs/RASPBERRY.md, "Bluetooth") ----
HCI=""
for h in /sys/class/bluetooth/hci*; do
    [ -e "$h" ] || continue
    case "$(readlink -f "$h")" in */serial*/bluetooth/*) HCI=$(basename "$h") ;; esac
done
if [ -n "$HCI" ]; then
    FW=$( (dmesg 2>/dev/null || journalctl -k -b --no-pager 2>/dev/null) | grep -m1 "$HCI: BCM.*hcd" | sed 's/^.*\] //')
    row PASS "Bluetooth controller" "$HCI on the UART${FW:+ ($FW)}"
    if systemctl is-active -q bluetooth.service; then row PASS "bluetoothd" "active"
    else row FAIL "bluetoothd" "$(systemctl is-active bluetooth.service)"; fi
    if systemctl is-active -q "lordos-rpi-bt@$HCI.service"; then row PASS "Bluetooth UART helper" "lordos-rpi-bt@$HCI: $(journalctl -b -u "lordos-rpi-bt@$HCI" -o cat --no-pager 2>/dev/null | grep -m1 'bt-uart:' | sed 's/^bt-uart: //')"
    else row WARN "Bluetooth UART helper" "lordos-rpi-bt@$HCI is $(systemctl is-active "lordos-rpi-bt@$HCI.service")"; fi
    if command -v btmgmt > /dev/null; then
        A=$(btmgmt --index "${HCI#hci}" info 2>/dev/null | sed -n 's/^[[:space:]]*addr \([0-9A-F:]*\).*/\1/p' | head -1)
        case "$A" in ""|43:45:C0:*|AA:AA:AA:AA:AA:AA|00:00:00:00:00:00) row FAIL "Bluetooth address" "'${A}' (the chip's default: no local-bd-address)" ;;
                     *) row PASS "Bluetooth address" "$A" ;; esac
    fi
elif [ -e /proc/device-tree/aliases/bluetooth ]; then row FAIL "Bluetooth controller" "the device tree has one but no hciN on a UART (dtoverlay=disable-bt? hci_uart?)"
else row INFO "Bluetooth controller" "none on this board"; fi

# ---- remote desktop: listening on its port only when enabled ----
RC=/etc/lordos/remote/config
EN=0; PORT=5673
if [ -f "$RC" ]; then
    EN=$(sed -n 's/^enabled=//p' "$RC" | head -1); PORT=$(sed -n 's/^port=//p' "$RC" | head -1)
fi
[ -n "$PORT" ] || PORT=5673
LIST=$(ss -Hltn 2>/dev/null | awk '{ print $4 }' | grep -E "[:.]$PORT\$" | head -1)
if [ "$EN" = 1 ]; then
    if [ -n "$LIST" ]; then row PASS "remote desktop" "enabled, listening on $LIST (https, login required)"
    else row FAIL "remote desktop" "enabled in $RC but nothing listens on $PORT"; fi
else
    if [ -z "$LIST" ]; then row PASS "remote desktop" "off, port $PORT closed"
    else row FAIL "remote desktop" "off in config but $LIST is listening"; fi
fi
if systemctl is-enabled -q ssh.service 2>/dev/null; then
    if systemctl is-active -q ssh.service; then row INFO "SSH server" "on (ssh switch file or RPI_SSH=1)"
    else row FAIL "SSH server" "enabled but $(systemctl is-active ssh.service)"; fi
else row INFO "SSH server" "off"; fi
if systemctl is-active -q lordos.service; then row PASS "lordos.service" "active"
else row FAIL "lordos.service" "$(systemctl is-active lordos.service)"; fi

# ---- the test suites (optional: needs the tree and the build packages) ----
if [ -n "$REPO" ]; then
    if [ ! -f "$REPO/server/Makefile" ]; then row FAIL "test-full" "$REPO is not a LordOS tree"
    else
        OWNER=$(stat -c %U "$REPO")
        LOG=/tmp/lordos-verify-tests.log
        run() { if [ "$(id -u)" = 0 ] && [ "$OWNER" != root ]; then su "$OWNER" -s /bin/sh -c "$1"; else sh -c "$1"; fi; }
        echo "verify-board.sh: building and running make -C server test-full as $OWNER (log $LOG)…" >&2
        if run "cd '$REPO' && make -C server -j$(nproc) && make -C server test-full" > "$LOG" 2>&1; then
            row PASS "make -C server test-full" "$(grep -E '[0-9]+ passed' "$LOG" | tail -1)"
        else
            row FAIL "make -C server test-full" "$(grep -E '[0-9]+ passed|FAIL|fail' "$LOG" | tail -1) (see $LOG)"
        fi
        # doc_evil.sh on its own: run_suites.sh only shows its output when it fails
        run "cd '$REPO/server' && sh tests/doc_evil.sh" > "$LOG.doc" 2>&1
        DE=$(grep -m1 '^doc-evil: ' "$LOG.doc" | sed 's/^doc-evil: //')
        if [ -z "$DE" ]; then row FAIL "doc_evil.sh" "did not run (see $LOG.doc)"
        elif [ "${DE%/*}" = "${DE#*/}" ]; then row PASS "doc_evil.sh" "$DE"
        else row FAIL "doc_evil.sh" "$DE (see $LOG.doc)"; fi
        SB=$(grep -c '^  ok  *sandbox-test' "$LOG")
        if grep -q 'skipped  sandbox-test' "$LOG"; then row FAIL "sandbox-test" "skipped (no Landlock)"
        elif [ "$SB" -ge 3 ]; then row PASS "sandbox-test" "app, app net, helper: ok"
        else row FAIL "sandbox-test" "$SB/3 ok (see $LOG)"; fi
    fi
else
    row INFO "test-full" "not run (pass --repo <LordOS tree> with the build packages installed)"
fi

echo
printf '%-5s | %-26s | %s\n' RESULT CHECK DETAIL
echo "------+----------------------------+--------------------------------------------------------------"
printf '%s' "$ROWS"
echo
echo "PASS $PASS · FAIL $FAIL · WARN/INFO $WARN"
[ "$FAIL" = 0 ]
