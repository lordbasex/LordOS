#!/bin/bash
# freepart_loop.sh — the real partitioning step of "install in free space" on a loop device, and proof
# by checksum that nothing that existed changed. Needs root and build/freepart-cli; only loop devices
# on an image in a new scratch folder are ever touched. Not part of `make test`: docs/ISO.md.
#
#   sudo sh -c 'cd server && bash tests/freepart_loop.sh'
set -u
cd "$(dirname "$0")/.." || exit 1
CLI=$PWD/build/freepart-cli
[ -x "$CLI" ] || { echo "needs build/freepart-cli"; exit 2; }
W=$(mktemp -d "$PWD/build/freepart.XXXXXX") || exit 1
IMG=$W/disk.img LOOP='' MNT=$W/mnt
mkdir "$MNT"
pass=0 fail=0
ok()  { pass=$((pass + 1)); echo "  ok      $1"; }
bad() { fail=$((fail + 1)); echo "  FAILED  $1"; }
want() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: wanted [$2], got [$3]"; fi; }
detach() { [ -n "$LOOP" ] && { umount "$MNT" 2>/dev/null; losetup -d "$LOOP"; LOOP=''; udevadm settle --timeout=5; }; }
trap 'detach; rm -rf "$W"' EXIT

SIZE=$((12 * 1024))            # MiB: 8 GB free after the ESP and the data partition
ESP_S=2048   ESP_E=$((2048 + 200 * 2048 - 1))
DATA_S=$((ESP_E + 1))  DATA_E=$((DATA_S + 300 * 2048 - 1))

attach() { LOOP=$(losetup --find --show -P "$IMG"); udevadm settle --timeout=5; L=${LOOP#/dev/}; }
sectors() { dd if="$IMG" bs=512 skip="$1" count="$2" 2>/dev/null | sha256sum | cut -d' ' -f1; }
# one table entry, from the image: both copies
entry() { dd if="$IMG" bs=128 skip=$((2 * 4 + $1 - 1 + 0)) count=1 2>/dev/null | sha256sum | cut -d' ' -f1; }
entry2() { local t; t=$(($(stat -c %s "$IMG") / 512 - 33)); dd if="$IMG" bs=128 skip=$((t * 4 + $1 - 1)) count=1 2>/dev/null | sha256sum | cut -d' ' -f1; }
state() { sectors "$ESP_S" $((ESP_E - ESP_S + 1)); sectors "$DATA_S" $((DATA_E - DATA_S + 1)); entry 1; entry 3; entry2 1; entry2 3; sectors 0 1; }
fullgpt() { local t; t=$(($(stat -c %s "$IMG") / 512)); sectors 0 34; sectors $((t - 33)) 33; }

echo "== build the image: ESP (1), data (3, with a known file), free space"
truncate -s ${SIZE}M "$IMG"
sgdisk -o -n 1:$ESP_S:$ESP_E -t 1:ef00 -c 1:EFI -n 3:$DATA_S:$DATA_E -t 3:8300 -c 3:data "$IMG" >/dev/null
attach
mkfs.vfat -F 32 -n EFI "${LOOP}p1" >/dev/null
mkfs.ext4 -q -F -L data "${LOOP}p3"
mount "${LOOP}p1" "$MNT"
mkdir -p "$MNT/EFI/BOOT" "$MNT/EFI/other"
head -c 300000 /dev/urandom >"$MNT/EFI/BOOT/BOOTX64.EFI"
head -c 300000 /dev/urandom >"$MNT/EFI/other/loader.efi"
( cd "$MNT" && find . -type f | sort | xargs sha256sum ) >"$W/esp.sums"
umount "$MNT"
mount "${LOOP}p3" "$MNT"
head -c 5000000 /dev/urandom >"$MNT/known-file"
K=$(sha256sum <"$MNT/known-file" | cut -d' ' -f1)
umount "$MNT"
sync; detach
sgdisk -p "$IMG" | tail -3
state >"$W/before"
fullgpt >"$W/gpt-before"
attach

echo "== refusals, none of which may write"
out=$("$CLI" probe "$L")
case "$out" in *why=install.free.needUefi*) ok "BIOS boot refused" ;;
  *why=-*) echo "  (this machine booted through UEFI: BIOS refusal not exercised)" ;; *) bad "BIOS boot: $out" ;; esac
out=$("$CLI" probe "$L" --uefi); echo "  $out"
case "$out" in *ptable=gpt*esps=1*why=-*) ok "qualifies with --uefi" ;; *) bad "probe: $out" ;; esac
S=$(echo "$out" | sed 's/.*free=\([0-9]*\)\.\.\([0-9]*\) .*/\1/'); E=$(echo "$out" | sed 's/.*free=\([0-9]*\)\.\.\([0-9]*\) .*/\2/')
want "region starts after the data partition on 1 MiB" 0 $((S % 2048))
want "region ends on 1 MiB" 2047 $((E % 2048))
[ "$S" -gt "$DATA_E" ] && ok "region is after every partition" || bad "region overlaps"
"$CLI" create "$L" --uefi --start $((S + 2048)) --end "$E" | tail -1 | grep -q install.free.diskChanged && ok "a stale region is refused" || bad "stale region"
"$CLI" create "$L" --uefi --start "$S" --end $((E - 2048)) | tail -1 | grep -q install.free.diskChanged && ok "a stale end is refused" || bad "stale end"
"$CLI" create "$L" | tail -1 | grep -q 'create install.free.needUefi' && ok "create refuses BIOS boot" || { [ -d /sys/firmware/efi ] || bad "create on BIOS"; }
mount -o ro "${LOOP}p3" "$MNT"; "$CLI" create "$L" --uefi | tail -1 | grep -q install.free.busy && ok "a mounted partition is refused" || bad "busy"; umount "$MNT"
sync; detach; fullgpt >"$W/gpt-now"
cmp -s "$W/gpt-before" "$W/gpt-now" && ok "the table is byte-identical after the refusals" || bad "a refusal wrote"
attach

for f in 1 2 3; do
    echo "== failure $f: sgdisk wrote / mkfs fails / sgdisk itself refuses"
    "$CLI" create "$L" --uefi --fail $f | sed 's/^/  /'
    sync; detach; fullgpt >"$W/gpt-now"
    cmp -s "$W/gpt-before" "$W/gpt-now" && ok "failure $f: every table byte as it was" || bad "failure $f: table differs"
    attach
done

echo "== a later step fails (copy, grub): undo after the partition exists and is formatted"
"$CLI" create "$L" --uefi --undo | sed 's/^/  /'
sync; detach; fullgpt >"$W/gpt-now"
cmp -s "$W/gpt-before" "$W/gpt-now" && ok "undo: every table byte as it was" || bad "undo: table differs"
state >"$W/now"; cmp -s "$W/before" "$W/now" && ok "undo: partitions and entries as they were" || bad "undo: state differs"
attach

echo "== the real thing"
"$CLI" create "$L" --uefi | sed 's/^/  /'
sync; detach
echo "-- table now:"; sgdisk -p "$IMG" | tail -4; sgdisk -v "$IMG" | sed -n 2p
state >"$W/after"
paste -d' ' <(printf 'ESP-bytes\ndata-bytes\nentry-1\nentry-3\nbackup-entry-1\nbackup-entry-3\nsector-0\n') "$W/before" "$W/after" | while read -r n a b; do
    [ "$a" = "$b" ] && printf '  same     %-15s %s\n' "$n" "${a:0:32}" || printf '  CHANGED  %-15s %s %s\n' "$n" "${a:0:16}" "${b:0:16}"
done
cmp -s "$W/before" "$W/after" && ok "existing partitions, table entries and sector 0 unchanged (sha256)" || bad "something changed"
attach
want "partitions on the table" 3 "$(sgdisk -p "$IMG" | grep -c '^ *[0-9]\+  *[0-9]')"
want "new entry is number 2, between the others" "$S" "$(sgdisk -i 2 "$IMG" | sed -n 's/First sector: \([0-9]*\).*/\1/p')"
want "new entry ends at the region's end" "$E" "$(sgdisk -i 2 "$IMG" | sed -n 's/Last sector: \([0-9]*\).*/\1/p')"
want "new partition's type" 4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709 "$(sgdisk -i 2 "$IMG" | sed -n 's/Partition GUID code: \([-0-9A-F]*\).*/\1/p')"
want "new partition is ext4" ext4 "$(blkid -p -s TYPE -o value "${LOOP}p2")"
want "its label" LordOS "$(blkid -p -s LABEL -o value "${LOOP}p2")"
mount -o ro "${LOOP}p3" "$MNT"; want "the known file" "$K" "$(sha256sum <"$MNT/known-file" | cut -d' ' -f1)"; umount "$MNT"
mount -o ro "${LOOP}p1" "$MNT"; if ( cd "$MNT" && sha256sum --quiet -c "$W/esp.sums" ); then ok "ESP files intact"; else bad "ESP files"; fi; umount "$MNT"
out=$("$CLI" probe "$L" --uefi); echo "  $out"
case "$out" in *why=install.free.noRoom*) ok "second install refused: the room was used" ;; *) bad "second probe: $out" ;; esac

if [ -d /usr/lib/grub/x86_64-efi ] && command -v grub-install >/dev/null; then
    echo "== grub's arguments on the ESP, run from this machine (not the chroot of an install)"
    B=$W/boot; mkdir -p "$B"
    mount "${LOOP}p1" "$MNT"
    grub-install --target=x86_64-efi --efi-directory="$MNT" --boot-directory="$B" --bootloader-id=LordOS --no-nvram 2>&1 | tail -1 | sed 's/^/  /'
    [ -f "$MNT/EFI/LordOS/grubx64.efi" ] && ok "grub is in EFI/LordOS" || bad "grub missing"
    if ( cd "$MNT" && sha256sum --quiet -c "$W/esp.sums" ); then ok "fallback loader and the other system's folder unchanged"; else bad "grub touched them"; fi
    want "ESP files outside EFI/LordOS" "$(wc -l <"$W/esp.sums")" "$(cd "$MNT" && find . -type f -not -path './EFI/LordOS/*' | wc -l)"
    ls "$MNT/EFI" | sed 's/^/  EFI\//'
    umount "$MNT"
    out=$("$CLI" probe "$L" --uefi)
    case "$out" in *why=install.free.espHasLordOS*|*why=install.free.noRoom*) ok "EFI/LordOS now present: not offered again" ;; *) bad "$out" ;; esac
fi
detach

echo "== real disks that must be refused, read by the real sgdisk; none may change"
# refuse NAME CODE: the image as $IMG, already built; the probe must give CODE and no byte may change
refuse() {
    local before; before=$(sha256sum <"$IMG")
    attach
    out=$("$CLI" probe "$L" --uefi 2>/dev/null | tail -1)
    "$CLI" create "$L" --uefi --start 1026048 --end 25163775 >/dev/null 2>&1
    case "$out" in *"why=$2") ok "$1: $2" ;; *) bad "$1: $out" ;; esac
    detach
    [ "$before" = "$(sha256sum <"$IMG")" ] && ok "$1: image unchanged" || bad "$1: the image changed"
}
fresh() { rm -f "$IMG"; truncate -s ${SIZE}M "$IMG"; }
gptbase() { fresh; sgdisk -o -n 1:$ESP_S:$ESP_E -t 1:ef00 -c 1:EFI -n 3:$DATA_S:$DATA_E -t 3:8300 -c 3:data "$IMG" >/dev/null; }
fat() { attach; mkfs.vfat -F 32 "${LOOP}p1" >/dev/null; detach; }

fresh; refuse "blank disk" install.free.noTable
fresh; printf 'o\nn\np\n1\n\n+100M\nw\n' | fdisk "$IMG" >/dev/null 2>&1; refuse "MBR" install.free.mbr
gptbase; fat; sgdisk -h 3 "$IMG" >/dev/null; refuse "hybrid MBR" install.free.hybridMbr
gptbase; fat; dd if=/dev/urandom of="$IMG" bs=1 seek=1100 count=4 conv=notrunc 2>/dev/null; refuse "main table CRC broken" install.free.tableDamaged
gptbase; fat; dd if=/dev/zero of="$IMG" bs=512 seek=1 count=1 conv=notrunc 2>/dev/null; refuse "main header gone" install.free.tableDamaged
gptbase; fat; dd if=/dev/zero of="$IMG" bs=512 seek=$((SIZE * 2048 - 1)) count=1 conv=notrunc 2>/dev/null; refuse "backup header gone" install.free.tableDamaged
gptbase; fat; truncate -s $((SIZE + 100))M "$IMG"; refuse "disk grew, backup not at the end" install.free.tableDamaged
fresh; sgdisk -o -n 3:$DATA_S:$DATA_E -t 3:8300 "$IMG" >/dev/null; refuse "no ESP" install.free.noEsp
gptbase; sgdisk -n 2:$((DATA_E + 1)):+200M -t 2:ef00 "$IMG" >/dev/null; refuse "two ESPs" install.free.manyEsps
gptbase; attach; mkfs.ext4 -q -F "${LOOP}p1"; detach; refuse "ESP not FAT" install.free.espNotFat
gptbase; refuse "ESP never formatted" install.free.espNotFat
gptbase; fat; sgdisk -n 2:$((DATA_E + 1)):+15G -t 2:8300 "$IMG" 2>/dev/null >/dev/null || sgdisk -n 2:$((DATA_E + 1)):+10G -t 2:8300 "$IMG" >/dev/null; refuse "less than 8 GB free" install.free.noRoom

echo "== partitions off their alignment: a note from sgdisk -v, not a damaged table"
gptbase; fat; sgdisk -d 3 -n 3:$DATA_S:0 -t 3:8300 -c 3:data "$IMG" >/dev/null; refuse "full disk, last partition ends unaligned" install.free.noRoom
gptbase; fat; sgdisk -d 3 -n 3:$DATA_S:$((DATA_E - 5)) -t 3:8300 -c 3:data "$IMG" >/dev/null
sgdisk -v "$IMG" | grep -q 'Caution: Partition 3' && ok "sgdisk does note it" || bad "the image has no note"
attach
out=$("$CLI" probe "$L" --uefi | tail -1); echo "  $out"
case "$out" in *ptable=gpt*esps=1*why=-) ok "qualifies with a partition off its alignment" ;; *) bad "unaligned: $out" ;; esac
"$CLI" create "$L" --uefi | sed 's/^/  /'
sgdisk -p "$LOOP" | tail -3 | sed 's/^/  /'
want "unaligned: the new partition" 8304 "$(sgdisk -p "$LOOP" | awk '$1 == 2 { print $6 }')"
want "unaligned: the old one where it was" "$DATA_S $((DATA_E - 5))" "$(sgdisk -p "$LOOP" | awk '$1 == 3 { print $2, $3 }')"
detach

echo "== 4K logical sectors"
fresh
LOOP=$(losetup --find --show -P -b 4096 "$IMG"); L=${LOOP#/dev/}
sgdisk -o -n 1:256:+200M -t 1:ef00 -c 1:EFI -n 3:0:+300M -t 3:8300 -c 3:data "$LOOP" >/dev/null
udevadm settle --timeout=5; partx -u "$LOOP" 2>/dev/null; sleep 1
mkfs.vfat -F 32 -S 4096 "${LOOP}p1" >/dev/null
"$CLI" create "$L" --uefi | sed 's/^/  /'
udevadm settle --timeout=5
sgdisk -p "$LOOP" | tail -3 | sed 's/^/  /'
S4=$(sgdisk -i 2 "$LOOP" | sed -n 's/First sector: \([0-9]*\).*/\1/p'); E4=$(sgdisk -i 2 "$LOOP" | sed -n 's/Last sector: \([0-9]*\).*/\1/p')
want "4K: start on 1 MiB" 0 $((S4 % 256))
want "4K: end on 1 MiB" 255 $((E4 % 256))
want "4K: the kernel's start, in 512-byte units" $((S4 * 8)) "$(cat /sys/block/$L/${L}p2/start)"
want "4K: ext4" ext4 "$(blkid -p -s TYPE -o value "${LOOP}p2")"
detach
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]
