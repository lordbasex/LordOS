#!/bin/bash
# manualpart_loop.sh — the real steps of manual partitioning (src/install/manualpart.c) on a loop
# device: refusals that must not write, failures that must put the table back, and real plans proven by
# checksum (what the plan keeps is byte for byte what it was). Needs root and build/manualpart-cli; only
# loop devices on a sparse image in a new scratch folder are ever touched. Not part of `make test`:
# docs/ISO.md, "Manual partitioning".
#
#   sudo sh -c 'cd server && bash tests/manualpart_loop.sh'
set -u
cd "$(dirname "$0")/.." || exit 1
CLI=$PWD/build/manualpart-cli
[ -x "$CLI" ] || { echo "needs build/manualpart-cli"; exit 2; }
W=$(mktemp -d "$PWD/build/manualpart.XXXXXX") || exit 1
IMG=$W/disk.img LOOP='' MNT=$W/mnt
mkdir "$MNT"
pass=0 fail=0
ok()  { pass=$((pass + 1)); echo "  ok      $1"; }
bad() { fail=$((fail + 1)); echo "  FAILED  $1"; }
want() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: wanted [$2], got [$3]"; fi; }
detach() { [ -n "$LOOP" ] && { umount "$MNT" 2>/dev/null; losetup -d "$LOOP"; LOOP=''; udevadm settle --timeout=5; }; }
trap 'detach; rm -rf "$W"' EXIT

MIB=2048 GIB=2097152
SIZE=$((24 * 1024))                             # MiB, sparse
P1S=2048;              P1E=$((P1S + 200 * MIB - 1))     # EFI, with another system's loader
P2S=$((P1E + 1));      P2E=$((P2S + GIB - 1))           # an old /home, with a known file
P3S=$((P2E + 1));      P3E=$((P3S + 2 * GIB - 1))       # something to delete
P4S=$((P3E + 1));      P4E=$((P4S + 9 * GIB - 1))       # an old system, to reinstall over
FREES=$((P4E + 1))

attach() { LOOP=$(losetup --find --show -P "$IMG"); udevadm settle --timeout=5; L=${LOOP#/dev/}; }
sectors() { dd if="$IMG" bs=512 skip="$1" count="$2" 2>/dev/null | sha256sum | cut -d' ' -f1; }
entry() { dd if="$IMG" bs=128 skip=$((2 * 4 + $1 - 1)) count=1 2>/dev/null | sha256sum | cut -d' ' -f1; }
fullgpt() { local t; t=$(($(stat -c %s "$IMG") / 512)); sectors 0 34; sectors $((t - 33)) 33; }
esp_state() { sectors $P1S $((P1E - P1S + 1)); }
home_state() { sectors $P2S $((P2E - P2S + 1)); }
tag() { "$CLI" probe "$L" --uefi | sed -n 's/^probe tag=\([^ ]*\) .*/\1/p'; }
last() { "$CLI" probe "$L" --uefi | sed -n 's/.* last=\([0-9]*\).*/\1/p'; }

build() {
    rm -f "$IMG"; truncate -s ${SIZE}M "$IMG"
    sgdisk -o -n 1:$P1S:$P1E -t 1:ef00 -c 1:EFI -n 2:$P2S:$P2E -t 2:8302 -c 2:home \
              -n 3:$P3S:$P3E -t 3:8300 -c 3:old -n 4:$P4S:$P4E -t 4:8304 -c 4:oldroot "$IMG" >/dev/null
    attach
    mkfs.vfat -F 32 -n EFI "${LOOP}p1" >/dev/null
    mkfs.ext4 -q -F -L home "${LOOP}p2"
    mkfs.ext4 -q -F -L old "${LOOP}p3"
    mkfs.ext4 -q -F -L oldroot "${LOOP}p4"
    mount "${LOOP}p1" "$MNT"
    mkdir -p "$MNT/EFI/BOOT" "$MNT/EFI/other"
    head -c 300000 /dev/urandom >"$MNT/EFI/BOOT/BOOTX64.EFI"
    head -c 300000 /dev/urandom >"$MNT/EFI/other/loader.efi"
    umount "$MNT"
    mount "${LOOP}p2" "$MNT"
    mkdir -p "$MNT/alice"
    head -c 5000000 /dev/urandom >"$MNT/alice/known-file"
    umount "$MNT"
    sync; detach
}

echo "== build the image: EFI (1), an old /home (2), something to delete (3), an old system (4), free space"
build
sgdisk -p "$IMG" | tail -5
fullgpt >"$W/gpt-before"; esp_state >"$W/esp-before"; home_state >"$W/home-before"
attach
"$CLI" probe "$L" --uefi | sed 's/^/  /'
T=$(tag); LAST=$(last)
ROOTE=$(( (LAST + 1) / MIB * MIB - 1 ))
[ -n "$T" ] && ok "a tag for the table: $T" || bad "no tag"
"$CLI" probe "$L" --uefi | grep -q '^loader 1 other$' && ok "the other system's loader folder is seen (EFI/BOOT left out)" || bad "loaders"
"$CLI" probe "$L" --uefi | grep -q '^part 2 .* fs=ext4$' && ok "file system of each partition" || bad "fs types"

echo "== refusals, none of which may write"
r() { local name=$1 code=$2; shift 2; out=$("$CLI" apply "$L" "$@" | tail -1)
      case "$out" in "apply $code "*) ok "$name: $code" ;; *) bad "$name: $out" ;; esac; }
out=$("$CLI" probe "$L" | head -1)
case "$out" in *why=install.manual.needUefi*) ok "BIOS boot refused" ;; *why=-*) echo "  (booted through UEFI: BIOS refusal not exercised)" ;; *) bad "BIOS: $out" ;; esac
r "a stale tag" install.manual.diskChanged --uefi --tag 00-00 --root 4 --esp 1
r "no tag" install.manual.diskChanged --uefi --root 4 --esp 1
r "no /" install.manual.noRoot --uefi --tag "$T" --esp 1
r "no EFI partition" install.manual.noEsp --uefi --tag "$T" --root 4
r "/ on the EFI partition" install.manual.espForData --uefi --tag "$T" --root 1 --esp 1
r "EFI role on a data partition" install.manual.notEsp --uefi --tag "$T" --root 4 --esp 3
r "a partition twice" install.manual.usedTwice --uefi --tag "$T" --root 4 --home 4 --esp 1
r "a deleted partition in use" install.manual.deletedInUse --uefi --tag "$T" --root 4 --esp 1 --delete 4
r "/ too small" install.manual.rootTooSmall --uefi --tag "$T" --root 2 --esp 1
r "new / over a kept partition" install.manual.overlap --uefi --tag "$T" --esp 1 --create "root:$P4S:$ROOTE"
r "new / misaligned" install.manual.notAligned --uefi --tag "$T" --esp 1 --create "root:$((FREES + 1)):$ROOTE"
r "new / past the end" install.manual.outside --uefi --tag "$T" --esp 1 --create "root:$FREES:$((ROOTE + MIB))"
r "a swap partition and a swap file" install.manual.twoSwaps --uefi --tag "$T" --root 4 --esp 1 --swap 3 --swap-file 2048
mount -o ro "${LOOP}p3" "$MNT"; r "something mounted" install.manual.busy --uefi --tag "$T" --root 4 --esp 1; umount "$MNT"
sync; detach; fullgpt >"$W/gpt-now"
cmp -s "$W/gpt-before" "$W/gpt-now" && ok "the table is byte-identical after the refusals" || bad "a refusal wrote"
attach

for f in 1 2; do
    echo "== failure $f (after sgdisk wrote / before the first format), a plan in free space only: the table comes back"
    "$CLI" apply "$L" --uefi --tag "$T" --esp 1 --create "root:$FREES:$((FREES + 9 * GIB - 1)),swap:$((FREES + 9 * GIB)):$ROOTE" --fail $f | sed 's/^/  /'
    sync; detach; fullgpt >"$W/gpt-now"
    cmp -s "$W/gpt-before" "$W/gpt-now" && ok "failure $f: every table byte as it was" || bad "failure $f: table differs"
    attach
done
echo "== a later step fails (copy, grub) after a free-space-only plan: undo"
"$CLI" apply "$L" --uefi --tag "$T" --esp 1 --create "root:$FREES:$ROOTE" --undo | sed 's/^/  /'
sync; detach; fullgpt >"$W/gpt-now"
cmp -s "$W/gpt-before" "$W/gpt-now" && ok "undo: every table byte as it was" || bad "undo: table differs"
[ "$(esp_state)" = "$(cat "$W/esp-before")" ] && [ "$(home_state)" = "$(cat "$W/home-before")" ] && ok "undo: EFI and /home bytes as they were" || bad "undo: data changed"
attach

echo "== a plan that deletes and fails before the first format: the table is not put back (by design)"
out=$("$CLI" apply "$L" --uefi --tag "$T" --esp 1 --home 2 --delete 3 --create "swap:$P3S:$((P3S + GIB - 1)),root:$FREES:$ROOTE" --fail 2 | tail -1)
case "$out" in "apply install.formatRootFailed "*"undo=none") ok "reported, with nothing left to undo" ;; *) bad "$out" ;; esac
sync; detach; fullgpt >"$W/gpt-now"
cmp -s "$W/gpt-before" "$W/gpt-now" && bad "the table should be the new one" || ok "the new table stays"
build; attach; T=$(tag)
sync; esp_state >"$W/esp-before"; home_state >"$W/home-before"

echo "== the real thing: delete 3, swap in its place, / in free space, keep /home (2) and the EFI partition (1)"
out=$("$CLI" apply "$L" --uefi --tag "$T" --esp 1 --home 2 --delete 3 --create "root:$FREES:$ROOTE,swap:$P3S:$((P3S + GIB - 1))" | tail -1)
echo "  $out"
case "$out" in "apply ok root=${LOOP}p5 home=${LOOP}p2 swap=${LOOP}p6 esp=${LOOP}p1 homefs=ext4 espnum=1 espformatted=0 "*) ok "applied" ;; *) bad "apply: $out" ;; esac
sync; detach
sgdisk -p "$IMG" | tail -5 | sed 's/^/  /'
sgdisk -v "$IMG" | grep -q "No problems found" && ok "sgdisk -v: no problems" || bad "sgdisk -v"
want "EFI partition bytes unchanged" "$(cat "$W/esp-before")" "$(esp_state)"
want "/home bytes unchanged" "$(cat "$W/home-before")" "$(home_state)"
want "entry 3 is empty" "$(head -c 128 /dev/zero | sha256sum | cut -d' ' -f1)" "$(entry 3)"
want "the new / is number 5" "$FREES $ROOTE" "$(sgdisk -i 5 "$IMG" | sed -n 's/First sector: \([0-9]*\).*/\1/p;s/Last sector: \([0-9]*\).*/\1/p' | paste -sd' ')"
want "its type" 4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709 "$(sgdisk -i 5 "$IMG" | sed -n 's/Partition GUID code: \([-0-9A-F]*\).*/\1/p')"
want "swap is number 6, of type Linux swap" 0657FD6D-A4AB-43C4-84E5-0933C84B4F4F "$(sgdisk -i 6 "$IMG" | sed -n 's/Partition GUID code: \([-0-9A-F]*\).*/\1/p')"
want "partition 4 untouched" "$P4S" "$(sgdisk -i 4 "$IMG" | sed -n 's/First sector: \([0-9]*\).*/\1/p')"
attach
want "/ is ext4" "ext4 LordOS" "$(blkid -p -s TYPE -o value "${LOOP}p5") $(blkid -p -s LABEL -o value "${LOOP}p5")"
want "swap is swap" swap "$(blkid -p -s TYPE -o value "${LOOP}p6")"
want "partition 4 still has its old system" oldroot "$(blkid -p -s LABEL -o value "${LOOP}p4")"
mount -o ro "${LOOP}p1" "$MNT"; [ -f "$MNT/EFI/other/loader.efi" ] && ok "the other loader is still there" || bad "loader gone"; umount "$MNT"
detach

echo "== reinstall over partition 4: no table change, /home and EFI kept"
build; attach; T=$(tag)
fullgpt >"$W/gpt-before"; esp_state >"$W/esp-before"; home_state >"$W/home-before"
out=$("$CLI" apply "$L" --uefi --tag "$T" --root 4 --esp 1 --home 2 | tail -1); echo "  $out"
case "$out" in "apply ok root=${LOOP}p4 "*) ok "applied" ;; *) bad "$out" ;; esac
want "/ was formatted" LordOS "$(blkid -p -s LABEL -o value "${LOOP}p4")"
sync; detach
want "the table is byte-identical" "$(cat "$W/gpt-before")" "$(fullgpt)"
want "EFI partition bytes unchanged" "$(cat "$W/esp-before")" "$(esp_state)"
want "/home bytes unchanged" "$(cat "$W/home-before")" "$(home_state)"
attach

echo "== formatting the EFI partition is only done when asked"
out=$("$CLI" apply "$L" --uefi --tag "$T" --root 4 --esp 1 --esp-format --home 2 --home-format | tail -1); echo "  $out"
case "$out" in *"espformatted=1"*) ok "formatted on request" ;; *) bad "$out" ;; esac
mount -o ro "${LOOP}p1" "$MNT"; [ -d "$MNT/EFI" ] && bad "EFI folder survived a format" || ok "the old loaders are gone, as asked"; umount "$MNT"
want "/home formatted on request" home "$(blkid -p -s LABEL -o value "${LOOP}p2")"
mount -o ro "${LOOP}p2" "$MNT"; [ -e "$MNT/alice" ] && bad "old /home files survived" || ok "old /home files gone"; umount "$MNT"
detach

echo "== a new EFI partition and / in free space"
build; attach; T=$(tag)
ES=$FREES; EE=$((FREES + 512 * MIB - 1))
out=$("$CLI" apply "$L" --uefi --tag "$T" --create "esp:$ES:$EE,root:$((EE + 1)):$ROOTE" | tail -1); echo "  $out"
case "$out" in "apply ok root=${LOOP}p6 "*"esp=${LOOP}p5 "*"espformatted=1"*) ok "applied" ;; *) bad "$out" ;; esac
want "the new EFI partition's type" C12A7328-F81F-11D2-BA4B-00A0C93EC93B "$(sgdisk -i 5 "$LOOP" | sed -n 's/Partition GUID code: \([-0-9A-F]*\).*/\1/p')"
want "and FAT" vfat "$(blkid -p -s TYPE -o value "${LOOP}p5")"
out=$("$CLI" probe "$L" --uefi | head -1)
case "$out" in *why=-*) ok "two EFI partitions: still offered" ;; *) bad "$out" ;; esac
detach

echo "== 4K logical sectors"
rm -f "$IMG"; truncate -s ${SIZE}M "$IMG"
LOOP=$(losetup --find --show -P -b 4096 "$IMG"); L=${LOOP#/dev/}
sgdisk -o -n 1:256:+200M -t 1:ef00 -c 1:EFI "$LOOP" >/dev/null
udevadm settle --timeout=5; partx -u "$LOOP" 2>/dev/null; sleep 1
mkfs.vfat -F 32 -S 4096 "${LOOP}p1" >/dev/null
T=$(tag); LAST=$(last)
S4=$(( (256 + 51200 + 255) / 256 * 256 )); E4=$(( (LAST + 1) / 256 * 256 - 1 ))
out=$("$CLI" apply "$L" --uefi --tag "$T" --esp 1 --create "root:$S4:$E4" | tail -1); echo "  $out"
case "$out" in "apply ok "*) ok "4K: applied" ;; *) bad "4K: $out" ;; esac
want "4K: the kernel's start, in 512-byte units" $((S4 * 8)) "$(cat /sys/block/$L/${L}p2/start)"
want "4K: ext4" ext4 "$(blkid -p -s TYPE -o value "${LOOP}p2")"
detach
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]
