#!/bin/sh
# build-kernel.sh — the LordOS Raspberry Pi kernel: Raspberry Pi's own kernel source and defconfig,
# plus what LordOS needs that it lacks (Landlock, the BPF LSM and BTF). See docs/RASPBERRY.md,
# "The LordOS kernel".
#
# Usage (on an arm64 Debian 13, e.g. the Pi itself, about 75 min per flavour on a Pi 5; or cross on
# an x86-64 Debian 13 with gcc-aarch64-linux-gnu; ~10 GB free in the work folder while one flavour
# builds: BTF needs the DWARF debug info):
#   tools/rpi/build-kernel.sh [work folder] [2712|v8|all]     # default /var/tmp/lordos-kernel 2712
#
# Result: <work>/out/linux-image-<release>_<version>_arm64.deb (+ linux-headers-…), where
#   <release> = <upstream version>-lordos.<rev>-rpi-2712   (Pi 5: bcm2712_defconfig, 16K pages)
#               <upstream version>-lordos.<rev>-rpi-v8     (Pi 4/400/CM4: bcm2711_defconfig, 4K pages)
# <rev> is LORDOS_KREV, so each revision is its own kernel release (own /lib/modules, own vmlinuz):
# the previous one stays installed and bootable next to it, as with Debian's ABI bumps. Revisions 1
# and 2 (0.8.0 to 0.8.3) were "-lordos-rpi-<flavour>"; "-lordos.<rev>" sorts after that in sort -V
# (the hook's "newest"), "-lordos<rev>" or "-<rev>-lordos" would sort before it.
# The "-rpi-<flavour>" suffix is what raspi-firmware's kernel hook keys on (it copies the newest
# vmlinuz-*-rpi-2712 to kernel_2712.img and the device trees to /boot/firmware), so the package
# installs exactly like Raspberry Pi's own. tools/rpi/build-img.sh picks these .debs up from
# $KERNEL_DEBS (default /var/tmp/lordos-kernel/out).
#
# Variables: RPI_KERNEL_REPO (default the Raspberry Pi GitHub tree), RPI_KERNEL_BRANCH (rpi-6.18.y),
# RPI_KERNEL_REF (a commit to pin; default the branch head), LORDOS_KREV (package revision: 3 since
# 0.8.4, sock_diag SOCK_DESTROY; 2 was 0.8.1's, the BPF LSM and BTF; 1 was 0.8.0's),
# JOBS (default nproc). Building packages needs: build-essential bc bison flex libssl-dev
# libelf-dev libdw-dev dwarves (pahole, for BTF) cpio kmod rsync dpkg-dev debhelper git.
set -eu

WORK=${1:-/var/tmp/lordos-kernel}
FLAVOURS=${2:-2712}
REPO=${RPI_KERNEL_REPO:-https://github.com/raspberrypi/linux.git}
BRANCH=${RPI_KERNEL_BRANCH:-rpi-6.18.y}
REF=${RPI_KERNEL_REF:-}
KREV=${LORDOS_KREV:-3}
JOBS=${JOBS:-$(nproc)}
SRC="$WORK/linux"
OUT="$WORK/out"
[ "$FLAVOURS" = all ] && FLAVOURS="2712 v8"

case "$(uname -m)" in
    aarch64|arm64) CROSS="" ;;
    *) command -v aarch64-linux-gnu-gcc >/dev/null || {
           echo "build-kernel.sh: not arm64 and no aarch64-linux-gnu-gcc (gcc-aarch64-linux-gnu)" >&2; exit 1; }
       # the headers package builds its host tools (fixdep, …) for arm64: needs the arm64 libc headers
       [ -f /usr/aarch64-linux-gnu/include/sys/types.h ] || {
           echo "build-kernel.sh: no arm64 libc headers (libc6-dev-arm64-cross)" >&2; exit 1; }
       # cross build (x86-64 Debian 13, e.g. VM 116): dpkg-buildpackage would ask for the arm64
       # build dependencies (libssl-dev:arm64, …); the kernel only needs the native ones, so -d
       CROSS="CROSS_COMPILE=aarch64-linux-gnu- DPKG_FLAGS=-d" ;;
esac
for t in git make gcc bc bison flex dpkg-deb dpkg-buildpackage dh rsync cpio pahole readelf; do
    command -v "$t" >/dev/null || { echo "build-kernel.sh: missing $t" >&2; exit 1; }
done

# raspi-firmware's kernel hook copies overlays/README along with the overlays and fails without it;
# Raspberry Pi's own packages ship it, `make dtbs_install` doesn't: added to the package here.
add_overlay_readme() {   # .deb, kernel release
    dpkg-deb -c "$1" | grep -c "usr/lib/linux-image-$2/overlays/README\$" > /dev/null && return 0
    R="$WORK/repack"
    rm -rf "$R"; dpkg-deb -R "$1" "$R"
    install -m 644 "$SRC/arch/arm/boot/dts/overlays/README" "$R/usr/lib/linux-image-$2/overlays/README"
    [ -f "$R/DEBIAN/md5sums" ] && (cd "$R" && md5sum "usr/lib/linux-image-$2/overlays/README" >> DEBIAN/md5sums)
    dpkg-deb --root-owner-group -Zxz -b "$R" "$1" > /dev/null
    rm -rf "$R"
    echo "   added overlays/README to $(basename "$1")"
}

mkdir -p "$WORK" "$OUT"
if [ ! -f "$SRC/Makefile" ]; then
    echo "== cloning $REPO ($BRANCH)"
    git clone --depth 1 -b "$BRANCH" "$REPO" "$SRC"
fi
if [ -n "$REF" ]; then
    git -C "$SRC" fetch --depth 1 origin "$REF"
    git -C "$SRC" checkout -q FETCH_HEAD
fi
COMMIT=$(git -C "$SRC" rev-parse HEAD)
KV=$(make -s -C "$SRC" kernelversion)
echo "== Raspberry Pi kernel $KV ($BRANCH @ ${COMMIT%"${COMMIT#????????????}"})"

for FL in $FLAVOURS; do
    case "$FL" in
        2712) DEFCONFIG=bcm2712_defconfig ;;
        v8)   DEFCONFIG=bcm2711_defconfig ;;
        *) echo "build-kernel.sh: unknown flavour $FL (2712, v8 or all)" >&2; exit 1 ;;
    esac
    B="$WORK/build-$FL"
    LV="-lordos.$KREV-rpi-$FL"
    echo "== $FL: $DEFCONFIG -> $KV$LV"
    mkdir -p "$B"
    # shellcheck disable=SC2086
    make -s -C "$SRC" O="$B" ARCH=arm64 $CROSS "$DEFCONFIG"
    cp "$B/.config" "$B/config.defconfig"
    # ---- the LordOS changes (documented in docs/RASPBERRY.md; keep them in sync) ----
    # Landlock: filesystem and TCP confinement of apps (.lpk) and helpers (lord-doc, lord-media,
    # lord-maild): server/src/sec/sandbox.c. Raspberry Pi's defconfig leaves it out. Yama: the
    # lordos package's kernel.yama.ptrace_scope = 1 (as on a PC). The BPF LSM: see below.
    "$SRC/scripts/config" --file "$B/.config" \
        --enable SECURITY \
        --enable SECURITYFS \
        --enable SECURITY_NETWORK \
        --enable SECURITY_PATH \
        --enable SECURITY_LANDLOCK \
        --set-str LSM "landlock,lockdown,yama,integrity,bpf" \
        --enable BPF_LSM \
        --enable SECURITY_YAMA \
        --set-str LOCALVERSION "$LV" \
        --disable LOCALVERSION_AUTO
    # already in the defconfig, required by LordOS; enforced so a future defconfig can't drop them:
    # seccomp (sandbox.c), cgroup v2 + BPF (per-app limits, lord-netperm's cgroup/connect hooks)
    "$SRC/scripts/config" --file "$B/.config" \
        --enable SECCOMP --enable SECCOMP_FILTER \
        --enable CGROUPS --enable MEMCG --enable CGROUP_PIDS --enable CGROUP_SCHED \
        --enable BPF_SYSCALL --enable BPF_JIT --enable CGROUP_BPF
    # sock_diag SOCK_DESTROY (0.8.3): when a rule changes to deny, lord-netperm destroys the app's open
    # TCP connections and connected UDP sockets so the app sees them end instead of a stall
    # (docs/FIREWALL.md, "Existing connections are cut"); Raspberry Pi's defconfig leaves it off
    "$SRC/scripts/config" --file "$B/.config" \
        --module INET_DIAG --module INET_TCP_DIAG --module INET_UDP_DIAG --enable INET_DIAG_DESTROY
    # BPF LSM + BTF (0.8.1): lord-netperm's per-cgroup LSM program on socket_create (the per-app
    # "No Bluetooth" of Linux apps, docs/FIREWALL.md) needs the bpf LSM and /sys/kernel/btf/vmlinux
    # (its attach point is a BTF id). BTF is generated by pahole from the DWARF debug info; the debug
    # info itself doesn't ship: modules are stripped (builddeb, keeping their .BTF) and the -dbg
    # package is not built (the nokerneldbg build profile below).
    "$SRC/scripts/config" --file "$B/.config" \
        --enable BPF_EVENTS \
        --disable DEBUG_INFO_NONE \
        --enable DEBUG_INFO \
        --enable DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT \
        --disable DEBUG_INFO_REDUCED \
        --disable DEBUG_INFO_SPLIT \
        --enable DEBUG_INFO_BTF \
        --enable DEBUG_INFO_BTF_MODULES
    # shellcheck disable=SC2086
    make -s -C "$SRC" O="$B" ARCH=arm64 $CROSS olddefconfig
    for o in SECURITY_LANDLOCK SECURITY_YAMA SECCOMP_FILTER CGROUP_BPF BPF_SYSCALL BPF_LSM DEBUG_INFO_BTF INET_DIAG_DESTROY; do
        grep -q "^CONFIG_$o=y" "$B/.config" || { echo "build-kernel.sh: CONFIG_$o did not stick" >&2; exit 1; }
    done
    grep -q '^CONFIG_LSM="landlock' "$B/.config" || { echo "build-kernel.sh: CONFIG_LSM did not stick" >&2; exit 1; }
    # the diff against Raspberry Pi's defconfig, kept next to the packages
    diff "$B/config.defconfig" "$B/.config" > "$OUT/config-$KV$LV.diff" || true
    cp "$B/.config" "$OUT/config-$KV$LV"
    echo "   config diff: $(grep -c '^[<>]' "$OUT/config-$KV$LV.diff") lines ($OUT/config-$KV$LV.diff)"

    T0=$(date +%s)
    rm -f "$WORK"/linux-*"$KV$LV"*.deb "$WORK"/linux-libc-dev_*.deb "$WORK"/linux-lordos-rpi-"$FL"_*
    # bindeb-pkg leaves the .debs in the parent of the build folder ($WORK)
    # shellcheck disable=SC2086
    # LOCALVERSION= (empty): without it setlocalversion appends "+" for a git tree that isn't at a
    # tag, giving "…-rpi-2712+", which raspi-firmware's hook doesn't recognize as a flavour
    # DEB_BUILD_PROFILES: no linux-image-*-dbg (the debug info is only there to make BTF)
    DEB_BUILD_PROFILES="pkg.linux-lordos-rpi-$FL.nokerneldbg" \
    make -C "$SRC" O="$B" ARCH=arm64 $CROSS -j"$JOBS" LOCALVERSION= \
        KDEB_PKGVERSION="$KV-$KREV+lordos" KDEB_SOURCENAME="linux-lordos-rpi-$FL" \
        KBUILD_BUILD_USER=lordos KBUILD_BUILD_HOST=lordos \
        bindeb-pkg > "$OUT/build-$FL.log" 2>&1 || { tail -40 "$OUT/build-$FL.log"; exit 1; }
    T1=$(date +%s)
    for d in "$WORK/linux-image-$KV$LV"_*.deb "$WORK/linux-headers-$KV$LV"_*.deb; do
        [ -f "$d" ] && mv "$d" "$OUT/"
    done
    rm -f "$WORK"/linux-libc-dev_*.deb "$WORK/linux-image-$KV$LV"-dbg_*.deb "$WORK"/linux-lordos-rpi-"$FL"_*
    # this revision's package only (out/ may still hold an older revision of the same kernel)
    IMGDEB="$OUT/linux-image-$KV${LV}_$KV-${KREV}+lordos_arm64.deb"
    [ -f "$IMGDEB" ] || {
        echo "build-kernel.sh: no $(basename "$IMGDEB"); built: $(ls "$WORK"/*.deb "$OUT"/*.deb 2>/dev/null)" >&2; exit 1; }
    add_overlay_readme "$IMGDEB" "$KV$LV"
    # BTF really in the kernel (vmlinux's .BTF section, what becomes /sys/kernel/btf/vmlinux)
    readelf -S "$B/vmlinux" | grep -q ' \.BTF ' ||
        { echo "build-kernel.sh: $B/vmlinux has no .BTF section" >&2; exit 1; }
    # the device trees and overlays where raspi-firmware's hook looks for them
    dpkg-deb -c "$IMGDEB" | grep -c "usr/lib/linux-image-$KV$LV/broadcom/bcm27.*\.dtb" > /dev/null ||
        { echo "build-kernel.sh: $IMGDEB has no device trees in /usr/lib/linux-image-$KV$LV/broadcom" >&2; exit 1; }
    dpkg-deb -c "$IMGDEB" | grep -c "usr/lib/linux-image-$KV$LV/overlays/.*\.dtbo" > /dev/null ||
        { echo "build-kernel.sh: $IMGDEB has no overlays" >&2; exit 1; }
    echo "   built in $(( (T1 - T0) / 60 )) min: $(basename "$IMGDEB") ($(du -h "$IMGDEB" | cut -f1))"
done
(cd "$OUT" && sha256sum linux-*.deb > SHA256SUMS)
printf 'source=%s\nbranch=%s\ncommit=%s\nversion=%s\n' "$REPO" "$BRANCH" "$COMMIT" "$KV" > "$OUT/SOURCE"
ls -l "$OUT"/*.deb
