#!/bin/sh
# vm_import_test.sh — Machines: what `lord-vm --create --import` refuses (docs/VIRTUALIZATION.md, "Phase 1
# implementation", Import). A crafted qcow2 can name a backing file or an external data file; if the import
# followed it, the new disk would hold a copy of a file the guest must never read. Also: a raw file named .qcow2,
# a qcow2 with a backing file hidden under a .img name, and a system disk swapped for one with a backing file
# after the machine was made (lord-vm refuses to start it).
#
#   sh tests/vm_import_test.sh BUILD_DIR
#
# Needs qemu-img and qemu-system-<this arch> (no KVM: nothing boots). One line per check; exit 1 on any FAIL.
set -u
BIN=$(cd "$1" && pwd) || exit 2
VM="$BIN/lord-vm"
W=$(mktemp -d /tmp/lvimp.XXXXXX) || exit 2
trap 'rm -rf "$W"' EXIT
pass=0 fail=0
ok()  { pass=$((pass + 1)); echo "PASS  $1"; }
bad() { fail=$((fail + 1)); echo "FAIL  $1${2:+: $2}"; }
command -v qemu-img >/dev/null || { echo "SKIP  qemu-img isn't installed"; exit 0; }

# the "secret": a file the person can read, with a marker the guest must never see
SECRET="$W/secret.raw"
head -c 1048576 /dev/zero >"$SECRET"
printf 'LORDOS-IMPORT-SECRET-MARKER' | dd of="$SECRET" conv=notrunc 2>/dev/null
has_secret() { qemu-img convert -q -O raw "$1" "$W/probe.raw" 2>/dev/null && grep -aq LORDOS-IMPORT-SECRET-MARKER "$W/probe.raw"; r=$?; rm -f "$W/probe.raw"; return $r; }

refused() {   # NAME IMAGE
    D="$W/m-$1.lordvm"
    if "$VM" --create "$D" --name "$1" --import "$2" --network isolated --vcpus 1 --memory 512 >"$W/$1.out" 2>&1; then
        bad "$1: refused" "$(cat "$W/$1.out")"
        [ -f "$D/disks/system.qcow2" ] && has_secret "$D/disks/system.qcow2" && bad "$1: the secret reached the new disk"
    else
        [ -f "$D/machine.json" ] || [ -f "$D/disks/system.qcow2" ] && bad "$1: nothing left behind" || ok "$1: refused ($(tail -1 "$W/$1.out" | cut -c1-90))"
    fi
}

qemu-img create -q -f qcow2 -F raw -b "$SECRET" "$W/backing.qcow2" 1M
refused backing-file "$W/backing.qcow2"
cp "$W/backing.qcow2" "$W/backing-as.img"
refused backing-file-named-img "$W/backing-as.img"
cp "$SECRET" "$W/data-copy.raw"
if qemu-img create -q -f qcow2 -o data_file="$W/data-copy.raw",data_file_raw=on "$W/datafile.qcow2" 1M 2>/dev/null; then
    refused external-data-file "$W/datafile.qcow2"
else echo "SKIP  external-data-file: this qemu-img can't make one"; fi
cp "$SECRET" "$W/raw-pretends.qcow2"
refused raw-named-qcow2 "$W/raw-pretends.qcow2"
cp "$SECRET" "$W/unknown.vmdk"
refused unknown-extension "$W/unknown.vmdk"
printf 'QFI\373\000\000\000\003' >"$W/short.qcow2"
refused short-header "$W/short.qcow2"
# a qcow2 header with the backing offset patched in by hand (qemu-img can't make it point anywhere odd)
qemu-img create -q -f qcow2 "$W/patched.qcow2" 1M
printf '\000\000\000\000\000\000\002\000\000\000\000\010' | dd of="$W/patched.qcow2" bs=1 seek=8 conv=notrunc 2>/dev/null
refused patched-backing-offset "$W/patched.qcow2"

# a good qcow2 and a good raw image: accepted, and the new disk has no backing file
qemu-img create -q -f qcow2 "$W/good.qcow2" 64M
D="$W/m-good.lordvm"
if "$VM" --create "$D" --name good --import "$W/good.qcow2" --network isolated --vcpus 1 --memory 512 >"$W/good.out" 2>&1; then
    if qemu-img info "$D/disks/system.qcow2" | grep -q "backing file"; then bad "good qcow2: new disk without backing"; else ok "good qcow2: imported, no backing file"; fi
else bad "good qcow2: imported" "$(cat "$W/good.out")"; fi
head -c 2097152 /dev/zero >"$W/good.img"
D2="$W/m-raw.lordvm"
if "$VM" --create "$D2" --name raw --import "$W/good.img" --network isolated --vcpus 1 --memory 512 >"$W/raw.out" 2>&1; then ok "good raw .img: imported"
else bad "good raw .img: imported" "$(cat "$W/raw.out")"; fi

# --set (Configure) on the raw machine, which is off: CPU, memory and network change, the disk grows and never
# shrinks, an ISO is attached and taken out, a held lock (a running machine) refuses every change
if [ -f "$D2/machine.json" ]; then
    if "$VM" --set "$D2" --vcpus 1 --memory 768 --network shared >"$W/set.out" 2>&1 && grep -q '"memory_mb": 768' "$D2/machine.json" && grep -q '"network": "shared"' "$D2/machine.json"
    then ok "set: memory and network changed"; else bad "set: memory and network changed" "$(cat "$W/set.out")"; fi
    if "$VM" --set "$D2" --disk 21 >"$W/grow.out" 2>&1 && qemu-img info "$D2/disks/system.qcow2" | grep -q "virtual size: 21 GiB"
    then ok "set: disk grown to 21 GB"; else bad "set: disk grown to 21 GB" "$(cat "$W/grow.out") $(qemu-img info "$D2/disks/system.qcow2" | grep virtual)"; fi
    if "$VM" --set "$D2" --disk 19 >"$W/shrink.out" 2>&1; then bad "set: disk never shrinks"; else ok "set: disk never shrinks ($(tail -1 "$W/shrink.out" | cut -c1-60))"; fi
    head -c 65536 /dev/zero >"$W/cd.iso"
    if "$VM" --set "$D2" --iso "$W/cd.iso" >/dev/null 2>&1 && grep -q '"iso"' "$D2/machine.json" && "$VM" --set "$D2" --iso none >/dev/null 2>&1 && ! grep -q '"iso"' "$D2/machine.json"
    then ok "set: ISO attached and taken out"; else bad "set: ISO attached and taken out"; fi
    if "$VM" --set "$D2" --memory 999999 >/dev/null 2>&1; then bad "set: memory out of range refused"; else ok "set: memory out of range refused"; fi
    ( flock -x 9; "$VM" --set "$D2" --vcpus 1 >"$W/locked.out" 2>&1; echo $? >"$W/locked.rc" ) 9>"$D2/lock"
    if [ "$(cat "$W/locked.rc")" = 3 ]; then ok "set: refused while the machine runs (lock held)"; else bad "set: refused while running" "$(cat "$W/locked.out")"; fi
fi

# the system disk swapped afterwards for one with a backing file: lord-vm refuses to start it
if [ -f "$D/disks/system.qcow2" ]; then
    cp "$W/backing.qcow2" "$D/disks/system.qcow2"
    timeout 30 "$VM" --start "$D" >"$W/start.out" 2>&1
    if "$VM" --status "$D" --json | grep -q '"reason":"start-failed"' && grep -q "system disk can't be used" "$D/log/lord-vm.log"; then ok "swapped system disk: start refused"
    else bad "swapped system disk: start refused" "$("$VM" --status "$D" --json) $(tail -3 "$D/log/lord-vm.log")"; fi
fi


# ---- machines of other virtualization apps (6.22): lord-vm --import-plan and --import-machine ----
# Every file of a source is hostile: a name that leaves the bundle, a symlink out, an XML declaration, a disk piece
# elsewhere must all be refused, and the secret must never reach a new disk. Merged snapshot layers must give the
# same bytes as the source's current state. The sources are only read.
GEN="$(dirname "$0")/vmimport_gen.py"
LIB="${W:?}/lib"; mkdir -p "$LIB"
planok() { "$VM" --import-plan "$1" >"$W/plan.json" 2>"$W/plan.err" && ! grep -q '"blockers":\[{' "$W/plan.json"; }
imp_refused() {   # LABEL SRC [TEXT]: refused by plan and import, nothing made, the secret nowhere
    if [ -z "${NOPLAN:-}" ] && planok "$2"; then bad "$1: the plan refuses" "$(cut -c1-300 "$W/plan.json")"; fi
    if "$VM" --import-machine "$2" "$LIB/x.lordvm" >"$W/imp.out" 2>&1; then bad "$1: refused" "$(tail -2 "$W/imp.out")"
        for f in "$LIB"/x.lordvm/disks/*.qcow2; do [ -f "$f" ] && has_secret "$f" && bad "$1: the secret reached the new disk"; done
        rm -rf "${LIB:?}/x.lordvm"
    elif [ -n "${3:-}" ] && ! grep -q "$3" "$W/imp.out"; then bad "$1: refused for the right reason" "$(tail -1 "$W/imp.out")"
    elif ls -d "$LIB"/x.lordvm "$LIB"/.x.lordvm.part-* >/dev/null 2>&1; then bad "$1: nothing left behind"
    else ok "$1: refused ($(grep -v '^20..-' "$W/imp.out" | tail -1 | cut -c10-100))"; fi
}
vmx() {   # DIR NAME FILENAME [EXTRA...]: a small .vmx with one disk
    mkdir -p "$1"
    { printf 'config.version = "8"\nvirtualHW.version = "19"\ndisplayName = "%s"\nguestOS = "debian12-64"\nmemsize = "512"\n' "$2"
      printf 'scsi0:0.present = "TRUE"\nscsi0:0.fileName = "%s"\n' "$3"; shift 3; for x in "$@"; do printf '%s\n' "$x"; done; } >"$1/m.vmx"
}
sums() { find "$1" -type f -exec sha256sum {} + | sort | sha256sum | cut -c1-16; }

# the secret as disks the bundles must not reach, beside them
qemu-img convert -q -f raw -O vmdk "$SECRET" "$W/secret.vmdk"
qemu-img convert -q -f raw -O vdi "$SECRET" "$W/secret.vdi"
qemu-img convert -q -f raw -O parallels "$SECRET" "$W/secret.hds"

B="$W/b1.vmwarevm"; vmx "$B" up "../secret.vmdk"
imp_refused "vmx: a disk named with .." "$B" "outside its folder"
B="$W/b2.vmwarevm"; vmx "$B" abs "$W/secret.vmdk"
imp_refused "vmx: a disk named by an absolute path (only its last part is looked for in the bundle)" "$B" "missing"
B="$W/b3.vmwarevm"; vmx "$B" link "disk.vmdk"; ln -s "$W/secret.vmdk" "$B/disk.vmdk"
imp_refused "vmx: a symlink that leads out of the bundle" "$B" "outside its folder"
B="$W/b4.vmwarevm"; vmx "$B" ext "disk.vmdk"
printf '# Disk DescriptorFile\nversion=1\ncreateType="monolithicFlat"\nRW 2048 FLAT "../secret.raw" 0\n' >"$B/disk.vmdk"
imp_refused "vmdk: an extent named with .." "$B" "folder"
B="$W/b5.vmwarevm"; vmx "$B" dev "disk.vmdk"
printf '# Disk DescriptorFile\nversion=1\ncreateType="monolithicFlat"\nRW 2048 FLAT "/dev/sda" 0\n' >"$B/disk.vmdk"
imp_refused "vmdk: an extent that is a device path" "$B" "folder"
B="$W/b6.vmwarevm"; vmx "$B" par "child.vmdk"
qemu-img create -q -f vmdk -b "$W/secret.vmdk" -F vmdk "$B/child.vmdk"
sed -i "s#^parentFileNameHint=.*#parentFileNameHint=\"../secret.vmdk\"#" "$B/child.vmdk" 2>/dev/null || true
imp_refused "vmdk: a snapshot whose parent is outside the bundle" "$B"
B="$W/b7.vmwarevm"; vmx "$B" lnext "disk.vmdk"
cp "$SECRET" "$W/out-flat.vmdk"; ln -s "$W/out-flat.vmdk" "$B/disk-flat.vmdk"
printf '# Disk DescriptorFile\nversion=1\ncreateType="monolithicFlat"\nRW 2048 FLAT "disk-flat.vmdk" 0\n' >"$B/disk.vmdk"
imp_refused "vmdk: an extent that is a symlink out of the bundle" "$B" "outside its folder"
B="$W/b8.vmwarevm"; vmx "$B" q "disk.vmdk"; cp "$W/backing.qcow2" "$B/disk.vmdk"
imp_refused "vmx: a qcow2 with a backing file under a .vmdk name" "$B" "backing"
B="$W/b9.vmwarevm"; vmx "$B" enc "disk.vmdk" 'encryption.keySafe = "vmware:key/list/x"'; qemu-img create -q -f vmdk "$B/disk.vmdk" 1M
imp_refused "vmx: an encrypted machine" "$B" "encrypted"
B="$W/b10.vmwarevm"; vmx "$B" susp "disk.vmdk" 'checkpoint.vmState = "m-1.vmss"'; qemu-img create -q -f vmdk "$B/disk.vmdk" 1M
imp_refused "vmx: a suspended machine" "$B" "suspended"
B="$W/b11.vmwarevm"; vmx "$B" lck "disk.vmdk"; qemu-img create -q -f vmdk "$B/disk.vmdk" 1M; mkdir "$B/disk.vmdk.lck"
imp_refused "vmx: the other app has it open" "$B" "open"

# XML: an external entity, and entity expansion, in a .vbox
mkdir -p "$W/x1"
printf '<?xml version="1.0"?>\n<!DOCTYPE VirtualBox [<!ENTITY s SYSTEM "file://%s">]>\n<VirtualBox><Machine uuid="{11111111-2222-3333-4444-555566667777}" name="&s;" OSType="Debian_64"><Hardware><Memory RAMSize="512"/></Hardware></Machine></VirtualBox>\n' "$SECRET" >"$W/x1/m.vbox"
imp_refused "vbox: an external entity (XXE)" "$W/x1/m.vbox" "DOCTYPE"
mkdir -p "$W/x2"
printf '<?xml version="1.0"?>\n<!DOCTYPE a [<!ENTITY a "aaaaaaaaaa"><!ENTITY b "&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;"><!ENTITY c "&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;">]>\n<VirtualBox><Machine name="&c;"/></VirtualBox>\n' >"$W/x2/m.vbox"
imp_refused "vbox: entity expansion" "$W/x2/m.vbox" "DOCTYPE"
mkdir -p "$W/x3"
printf '<?xml version="1.0"?><VirtualBox><Machine uuid="{11111111-2222-3333-4444-555566667777}" name="t" OSType="Debian_64"><MediaRegistry><HardDisks><HardDisk uuid="{aaaaaaaa-0000-0000-0000-000000000001}" location="../secret.vdi" format="VDI"/></HardDisks></MediaRegistry><Hardware><Memory RAMSize="512"/><StorageControllers><StorageController name="S" type="AHCI"><AttachedDevice type="HardDisk" port="0" device="0"><Image uuid="{aaaaaaaa-0000-0000-0000-000000000001}"/></AttachedDevice></StorageController></StorageControllers></Hardware></Machine></VirtualBox>\n' >"$W/x3/m.vbox"
imp_refused "vbox: a disk location with .." "$W/x3/m.vbox" "outside its folder"
mkdir -p "$W/x4.pvm/harddisk.hdd"
printf '<?xml version="1.0"?><ParallelsVirtualMachine><Identification><VmName>p</VmName></Identification><Hardware><Memory><RAM>512</RAM></Memory><Hdd id="0"><SystemName>harddisk.hdd</SystemName></Hdd></Hardware></ParallelsVirtualMachine>\n' >"$W/x4.pvm/config.pvs"
printf '<?xml version="1.0"?><Parallels_disk_image><Disk_Parameters><Disk_size>2048</Disk_size></Disk_Parameters><StorageData><Storage><Image><GUID>{1}</GUID><File>../../secret.hds</File></Image></Storage></StorageData></Parallels_disk_image>\n' >"$W/x4.pvm/harddisk.hdd/DiskDescriptor.xml"
imp_refused "pvs: a data file named with .." "$W/x4.pvm" "outside its folder"
# OVA: members with folders or "..", a link; an OVF whose disk is outside its folder
mkdir -p "$W/o1" && cp "$W/secret.vmdk" "$W/o1/disk.vmdk" && printf '<Envelope/>' >"$W/o1/m.ovf"
( cd "$W/o1" && tar --format=ustar -cf "$W/o-dir.ova" m.ovf ../o1/disk.vmdk 2>/dev/null )
imp_refused "ova: a member with a folder or .. in its name" "$W/o-dir.ova" "folder"
( cd "$W/o1" && ln -sf disk.vmdk l.vmdk && tar --format=ustar -cf "$W/o-link.ova" m.ovf l.vmdk )
imp_refused "ova: a member that is a link" "$W/o-link.ova" "link"
mkdir -p "$W/o2"
printf '<Envelope><References><File id="f" href="../secret.vmdk"/></References><DiskSection><Disk diskId="d" fileRef="f"/></DiskSection><VirtualSystem id="x"><VirtualHardwareSection><Item><ResourceType>4</ResourceType><VirtualQuantity>512</VirtualQuantity></Item><Item><ResourceType>17</ResourceType><HostResource>ovf:/disk/d</HostResource></Item></VirtualHardwareSection></VirtualSystem></Envelope>\n' >"$W/o2/m.ovf"
imp_refused "ovf: a disk href with .." "$W/o2/m.ovf" "outside its folder"

# merges: VMDK snapshots (qemu-img stacks them, each layer named by us), VDI and .hds snapshots (lord-vm merges them)
head -c 8388608 /dev/urandom >"$W/base8.raw"
t_same() { if [ "$(sums "$1")" = "$2" ]; then ok "$3: the source is unchanged"; else bad "$3: the source is unchanged"; fi; }
B="$W/m1.vmwarevm"; mkdir -p "$B"
qemu-img convert -q -f raw -O vmdk -o subformat=twoGbMaxExtentSparse "$W/base8.raw" "$B/d.vmdk"
qemu-img create -q -f vmdk -b d.vmdk -F vmdk "$B/d-000001.vmdk"
qemu-io -c "write -P 0xc1 2M 1M" "$B/d-000001.vmdk" >/dev/null
qemu-img create -q -f vmdk -b d-000001.vmdk -F vmdk "$B/d-000002.vmdk"
qemu-io -c "write -P 0xc2 5M 1M" "$B/d-000002.vmdk" >/dev/null
vmx "$B" chain "d-000002.vmdk"
before=$(sums "$B")
python3 -I "$GEN" expect "$W/base8.raw" "$W/m1.expect" 2:0xc1 5:0xc2
if "$VM" --import-machine "$B" "$LIB/m1.lordvm" >"$W/m1.out" 2>&1 && qemu-img convert -q -O raw "$LIB/m1.lordvm/disks/system.qcow2" "$W/m1.raw" && cmp -s "$W/m1.raw" "$W/m1.expect"
then ok "vmdk: a split base and two snapshots merged, byte for byte"; else bad "vmdk: two snapshots merged" "$(tail -2 "$W/m1.out")"; fi
t_same "$B" "$before" "vmdk"
if qemu-img info "$LIB/m1.lordvm/disks/system.qcow2" | grep -q "backing file"; then bad "vmdk: the new disk has no backing file"; else ok "vmdk: the new disk has no backing file"; fi

B="$W/m2"; mkdir -p "$B/Snapshots"
qemu-img convert -q -f raw -O vdi "$W/base8.raw" "$B/base.vdi"
U0=$(python3 -I "$GEN" vdi-uuid "$B/base.vdi")
U1=$(python3 -I "$GEN" vdi-diff "$B/base.vdi" "$B/Snapshots/{s1}.vdi" 1 3)
U2=$(python3 -I "$GEN" vdi-zero "$B/Snapshots/{s1}.vdi" "$B/Snapshots/{s2}.vdi" 1)
cat >"$B/m.vbox" <<EOF
<?xml version="1.0"?>
<VirtualBox xmlns="http://www.virtualbox.org/" version="1.19-linux">
  <Machine uuid="{11111111-2222-3333-4444-555566667777}" name="vdi chain" OSType="Debian_64">
    <MediaRegistry><HardDisks>
      <HardDisk uuid="{$U0}" location="base.vdi" format="VDI" type="Normal">
        <HardDisk uuid="{$U1}" location="Snapshots/{s1}.vdi" format="VDI"><HardDisk uuid="{$U2}" location="Snapshots/{s2}.vdi" format="VDI"/></HardDisk>
      </HardDisk>
    </HardDisks></MediaRegistry>
    <Hardware><CPU count="1"/><Memory RAMSize="512"/><Network><Adapter slot="0" enabled="true" MACAddress="080027A1B2C3" type="82540EM"><NAT/></Adapter></Network>
      <StorageControllers><StorageController name="SATA" type="AHCI"><AttachedDevice type="HardDisk" port="0" device="0"><Image uuid="{$U2}"/></AttachedDevice></StorageController></StorageControllers>
    </Hardware>
  </Machine>
</VirtualBox>
EOF
before=$(sums "$B")
python3 -I "$GEN" expect "$W/base8.raw" "$W/m2.expect" 1:0 3:0xa1
if "$VM" --import-machine "$B/m.vbox" "$LIB/m2.lordvm" >"$W/m2.out" 2>&1 && qemu-img convert -q -O raw "$LIB/m2.lordvm/disks/system.qcow2" "$W/m2.raw" && cmp -s "$W/m2.raw" "$W/m2.expect"
then ok "vdi: two differencing layers merged by lord-vm, byte for byte (a block replaced, one zeroed)"; else bad "vdi: differencing layers merged" "$(tail -2 "$W/m2.out")"; fi
t_same "$B" "$before" "vdi"
if grep -q '"disk_serial": "VB' "$LIB/m2.lordvm/machine.json" 2>/dev/null && grep -q '"mac": "08:00:27:a1:b2:c3"' "$LIB/m2.lordvm/machine.json"
then ok "vdi: the default SATA serial and the MAC carried"; else bad "vdi: serial and MAC carried"; fi

B="$W/m3.pvm"; mkdir -p "$B/harddisk.hdd"
qemu-img convert -q -f raw -O parallels "$W/base8.raw" "$B/harddisk.hdd/harddisk.hdd.0.{b}.hds"
python3 -I "$GEN" hds-delta "$B/harddisk.hdd/harddisk.hdd.0.{b}.hds" "$B/harddisk.hdd/harddisk.hdd.0.{d}.hds" 2 4
cat >"$B/harddisk.hdd/DiskDescriptor.xml" <<EOF
<?xml version='1.0' encoding='UTF-8'?>
<Parallels_disk_image Version="1.0"><Disk_Parameters><Disk_size>16384</Disk_size></Disk_Parameters>
<StorageData><Storage><Start>0</Start><End>16384</End>
<Image><GUID>{b}</GUID><Type>Compressed</Type><File>harddisk.hdd.0.{b}.hds</File></Image>
<Image><GUID>{d}</GUID><Type>Compressed</Type><File>harddisk.hdd.0.{d}.hds</File></Image></Storage></StorageData>
<Snapshots><Shot><GUID>{b}</GUID><ParentGUID>{00000000-0000-0000-0000-000000000000}</ParentGUID></Shot><Shot><GUID>{d}</GUID><ParentGUID>{b}</ParentGUID></Shot></Snapshots>
</Parallels_disk_image>
EOF
printf '<?xml version="1.0"?><ParallelsVirtualMachine><Identification><VmUuid>{deadbeef-0000-4000-8000-00000000c0de}</VmUuid><VmName>hds chain</VmName></Identification><Hardware><Cpu><Number>1</Number></Cpu><Memory><RAM>512</RAM></Memory><Hdd id="0"><Enabled>1</Enabled><SystemName>/elsewhere/harddisk.hdd</SystemName><InterfaceType>2</InterfaceType></Hdd><NetworkAdapter id="0"><Enabled>1</Enabled><MAC>001C42ABCDEF</MAC></NetworkAdapter></Hardware></ParallelsVirtualMachine>\n' >"$B/config.pvs"
before=$(sums "$B")
python3 -I "$GEN" expect "$W/base8.raw" "$W/m3.expect" 2:0xb0 4:0xb1
if "$VM" --import-machine "$B" "$LIB/m3.lordvm" >"$W/m3.out" 2>&1 && qemu-img convert -q -O raw "$LIB/m3.lordvm/disks/system.qcow2" "$W/m3.raw" && cmp -s "$W/m3.raw" "$W/m3.expect"
then ok "hds: a snapshot layer merged by lord-vm, byte for byte"; else bad "hds: snapshot layer merged" "$(tail -2 "$W/m3.out")"; fi
t_same "$B" "$before" "hds"
if grep -q '"id": "deadbeef-0000-4000-8000-00000000c0de"' "$LIB/m3.lordvm/machine.json" 2>/dev/null && grep -q '"mac": "00:1c:42:ab:cd:ef"' "$LIB/m3.lordvm/machine.json"
then ok "hds: the UUID and the MAC carried"; else bad "hds: UUID and MAC carried"; fi
# a layer whose block table points past its file
cp -r "$B" "$W/m4.pvm"
printf '\377\377\377\177' | dd of="$W/m4.pvm/harddisk.hdd/harddisk.hdd.0.{d}.hds" bs=1 seek=72 conv=notrunc 2>/dev/null
# (the plan reads headers only; the merge finds it, and leaves nothing)
NOPLAN=1 imp_refused "hds: a block table entry past the file's end" "$W/m4.pvm" "outside its file"

echo "vm-import: $pass passed, $fail failed"
[ "$fail" = 0 ]
