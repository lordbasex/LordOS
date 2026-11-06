#!/bin/sh
# vm_import_identity.sh — Machines' import (docs/VIRTUALIZATION.md 6.22): a machine brought over from another
# virtualization app keeps what its guest saw. From one Debian 13 "nocloud" image it makes a machine in each source
# format (a .vmx bundle with a split VMDK and a snapshot, a real .vbox with a VDI and a snapshot when VBoxManage is
# installed, its .ova export, a config.pvs bundle with an .hds disk, a loose VHDX), imports each with
# `lord-vm --import-machine`, boots it with KVM, and has the guest itself report on its serial line what it reads:
# the SMBIOS UUID (/sys/class/dmi/id/product_uuid), serial and vendor, every network card's MAC and driver, the
# disks' serials, BIOS or UEFI, sockets and cores. Each is compared with the value the source's settings imply.
#
#   sh tests/vm_import_identity.sh BUILD_DIR GUEST_IMAGE [WORK_DIR]
#
# Needs KVM, qemu-img, passt, python3; VBoxManage is optional (the .vbox and .ova cases are skipped without it). Runs
# one source at a time and removes each before the next, so it fits in about 4 GB. Every identifier is invented.
set -u
BIN=$(cd "$1" && pwd) || exit 2
IMG=$2
W=${3:-${TMPDIR:-/var/tmp}/lvg-id.$$}
. "$(dirname "$0")/vm_gate_lib.sh"
gate_begin
LIB=$W/lib; mkdir -p "$LIB"

# the guest's report, on its console, from a unit passed as a first-boot credential
UNIT=$(cat <<'EOF'
[Unit]
After=local-fs.target systemd-udev-settle.service
[Service]
Type=oneshot
ExecStart=/bin/sh -c 'sleep 4; u=$$(cat /sys/class/dmi/id/product_uuid); s=$$(cat /sys/class/dmi/id/product_serial); v=$$(cat /sys/class/dmi/id/sys_vendor | tr " " _); n=""; for i in /sys/class/net/*; do b=$${i##*/}; [ $$b = lo ] && continue; n="$$n$$(cat $$i/address)/$$(basename $$(readlink $$i/device/driver)),"; done; d=$$(lsblk -dno NAME,SERIAL | tr -s " " ":" | tr "\\n" ","); f=bios; [ -d /sys/firmware/efi ] && f=uefi; so=$$(lscpu | sed -n "s/^Socket(s): *//p"); co=$$(lscpu | sed -n "s/^Core(s) per socket: *//p"); echo "<3>LORDVM-ID uuid=$$u serial=$$s vendor=$$v nics=$$n disks=$$d fw=$$f sockets=$$so cores=$$co" >/dev/kmsg'
EOF
)
DROP=$(printf '[Unit]\nWants=lordvm-id.service\n')
umask 077
PW="lvi-$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')"
{ echo "firstboot.locale=C.UTF-8"; echo "firstboot.keymap=us"; echo "firstboot.timezone=UTC"; echo "passwd.plaintext-password.root=$PW"
  echo "binary:systemd.extra-unit.lordvm-id.service=$(printf '%s\n' "$UNIT" | base64 -w0)"
  echo "binary:systemd.unit-dropin.multi-user.target=$(printf '%s\n' "$DROP" | base64 -w0)"; } >"$W/creds"
umask 022

# the guest as a raw file, made again for each source and removed before its import (so a few GB are enough)
graw() { qemu-img convert -q -O raw "$IMG" "$W/g.raw"; }
graw || { bad "the guest image converts"; gate_end vm-import-identity; exit 1; }

# report KEY: one field of the guest's line
report() { sed -n "s/.*LORDVM-ID .*\b$1=\([^ ]*\).*/\1/p" "$D/log/serial.log" | head -n1 | tr -d '\r'; }
boot() {   # NAME SRC [IMPORT OPTIONS]: import, start, wait for the report, stop
    name=$1 src=$2; shift 2
    D=$LIB/$name.lordvm
    if "$VM" --import-machine "$src" "$D" "$@" >"$W/$name.imp" 2>&1; then ok "$name: imported ($(tail -1 "$W/$name.imp" | cut -c1-150))"
    else bad "$name: imported" "$(tail -3 "$W/$name.imp")"; return 1; fi
    "$VM" --start "$D" --detach --keep-running --credentials-file "$W/creds" >/dev/null 2>&1
    if wait_state "$D" running 60 && wait_serial "$D" LORDVM-ID 240; then ok "$name: booted, the guest reported: $(grep -a -o 'LORDVM-ID.*' "$D/log/serial.log" | head -n1 | cut -c11-400)"
    else bad "$name: booted and reported" "$(tail -c 600 "$D/log/serial.log" | tr -d '\r')"; "$VM" --poweroff "$D" >/dev/null 2>&1; return 1; fi
    "$VM" --poweroff "$D" >/dev/null 2>&1; wait_state "$D" stopped 30
}
same() {   # LABEL GOT WANT
    if [ "$2" = "$3" ]; then ok "$1: $2"; else bad "$1" "the guest read '$2', the source implies '$3'"; fi
}
has() {    # LABEL GOT PART
    case "$2" in *"$3"*) ok "$1: $3";; *) bad "$1" "the guest read '$2', no '$3'";; esac
}
done_with() { rm -rf "${LIB:?}/$1.lordvm"; }

echo "-- a .vmx bundle: split sparse VMDK, a second disk with a snapshot, two cards, UEFI, 1 socket × 2 cores"
B=$W/vmx.vmwarevm; mkdir -p "$B"
qemu-img convert -q -f raw -O vmdk -o subformat=twoGbMaxExtentSparse "$W/g.raw" "$B/sys.vmdk"
qemu-img create -q -f vmdk "$B/data.vmdk" 64M
qemu-img create -q -f vmdk -b data.vmdk -F vmdk "$B/data-000001.vmdk"
qemu-io -c "write -P 0x5a 1M 64k" "$B/data-000001.vmdk" >/dev/null
cat >"$B/Imported.vmx" <<'EOF'
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "19"
displayName = "Identity, vmx"
guestOS = "debian12-64"
firmware = "efi"
numvcpus = "2"
cpuid.coresPerSocket = "2"
memsize = "1024"
uuid.bios = "56 4d 12 34 56 78 9a bc-de f0 11 22 33 44 55 66"
serialNumber = "EX-VMX-0001"
ethernet0.present = "TRUE"
ethernet0.virtualDev = "e1000e"
ethernet0.addressType = "generated"
ethernet0.generatedAddress = "00:0c:29:ab:cd:ef"
ethernet1.present = "TRUE"
ethernet1.virtualDev = "vmxnet3"
ethernet1.addressType = "static"
ethernet1.address = "00:50:56:01:02:03"
scsi0.present = "TRUE"
scsi0:0.present = "TRUE"
scsi0:0.fileName = "sys.vmdk"
scsi0:1.present = "TRUE"
scsi0:1.fileName = "data-000001.vmdk"
EOF
rm -f "$W/g.raw"
if boot vmx "$B"; then
    # virtualHW 19 is SMBIOS 2.7: a Linux guest reads uuid.bios's first three fields little-endian
    same "vmx: SMBIOS UUID" "$(report uuid)" "34124d56-7856-bc9a-def0-112233445566"
    same "vmx: SMBIOS serial" "$(report serial)" "EX-VMX-0001"
    has "vmx: first card's MAC and model" "$(report nics)" "00:0c:29:ab:cd:ef/e1000e"
    has "vmx: second card's MAC and model" "$(report nics)" "00:50:56:01:02:03/vmxnet3"
    same "vmx: firmware" "$(report fw)" "uefi"
    same "vmx: sockets" "$(report sockets)" "1"
    same "vmx: cores per socket" "$(report cores)" "2"
    qemu-img convert -q -O raw "$D/disks/data-1.qcow2" "$W/d.raw"
    t "vmx: the second disk carries its snapshot's data" '[ "$(dd if="$W/d.raw" bs=65536 skip=16 count=1 2>/dev/null | od -An -tx1 | tr -d " \n" | cut -c1-8)" = 5a5a5a5a ]'
    rm -f "$W/d.raw"
    vs=$(qemu-img info "$D/disks/data-1.qcow2" | sed -n 's/^virtual size: .*(\([0-9]*\) bytes)/\1/p')
    same "vmx: the second disk's virtual size" "$vs" 67108864
fi
rm -rf "${B:?}"; done_with vmx

if command -v VBoxManage >/dev/null; then
    echo "-- a real .vbox (VBoxManage $(VBoxManage --version 2>/dev/null)): VDI with a snapshot, two cards, a hardware UUID, DMI settings"
    VB=$W/vbox; mkdir -p "$VB"
    export VBOX_USER_HOME="$W/vbox-home"
    VBoxManage createvm --name idvbox --uuid 11111111-2222-4333-8444-555566667777 --ostype Debian_64 --basefolder "$VB" --register >/dev/null
    VBoxManage modifyvm idvbox --memory 1024 --cpus 2 --firmware bios --nic1 nat --nic-type1 82540EM --mac-address1 0800271A2B3C \
        --nic2 intnet --nic-type2 virtio --mac-address2 0800271A2B3D --hardware-uuid 01234567-89ab-4def-8123-456789abcdef >/dev/null
    VBoxManage setextradata idvbox "VBoxInternal/Devices/pcbios/0/Config/DmiSystemVendor" "Example Systems"
    VBoxManage setextradata idvbox "VBoxInternal/Devices/pcbios/0/Config/DmiSystemSerial" "EX-VBOX-0001"
    graw; VBoxManage convertfromraw "$W/g.raw" "$VB/idvbox/disk.vdi" --format VDI >/dev/null; rm -f "$W/g.raw"
    VBoxManage storagectl idvbox --name SATA --add sata --controller IntelAhci >/dev/null
    VBoxManage storageattach idvbox --storagectl SATA --port 0 --device 0 --type hdd --medium "$VB/idvbox/disk.vdi" >/dev/null
    VBoxManage snapshot idvbox take before-import >/dev/null
    BU=$(VBoxManage showmediuminfo "$VB/idvbox/disk.vdi" | sed -n 's/^UUID: *//p' | head -n1)
    # VirtualBox's default SATA serial: "VB%08x-%08x" of the base image's UUID (its time_low, its last 4 bytes little-endian)
    SER=$(python3 -I -c 'import sys,uuid; b=uuid.UUID(sys.argv[1]).bytes; print("VB%08x-%08x" % (int.from_bytes(b[0:4],"big"), int.from_bytes(b[12:16],"little")))' "$BU")
    LE=0; grep -q 'SmbiosUuidLittleEndian enabled="true"' "$VB/idvbox/idvbox.vbox" && LE=1     # in the snapshot's copy too
    # SMBIOS 2.5: Linux reads the 16 bytes as stored; a new 7.x machine stores the hardware UUID little-endian
    if [ "$LE" = 1 ]; then WANT=67452301-ab89-ef4d-8123-456789abcdef; else WANT=01234567-89ab-4def-8123-456789abcdef; fi
    VBoxManage export idvbox -o "$W/idvbox.ova" >/dev/null 2>&1 || bad "vbox: exported as .ova"
    VBoxManage unregistervm idvbox >/dev/null 2>&1          # the files stay; VirtualBox lets go of them
    if boot vbox "$VB/idvbox/idvbox.vbox"; then
        same "vbox: SMBIOS UUID ($( [ "$LE" = 1 ] && echo stored little-endian || echo network order ))" "$(report uuid)" "$WANT"
        same "vbox: SMBIOS serial" "$(report serial)" "EX-VBOX-0001"
        same "vbox: SMBIOS vendor" "$(report vendor)" "Example_Systems"
        has "vbox: first card's MAC and model" "$(report nics)" "08:00:27:1a:2b:3c/e1000"
        has "vbox: second card's MAC and model" "$(report nics)" "08:00:27:1a:2b:3d/virtio"
        has "vbox: SATA disk serial" "$(report disks)" "sda:$SER"
        same "vbox: firmware" "$(report fw)" "bios"
    fi
    done_with vbox; rm -rf "${VB:?}" "${VBOX_USER_HOME:?}"
    if [ -f "$W/idvbox.ova" ]; then
        echo "-- its .ova export: the disk read in place inside the package"
        if boot ova "$W/idvbox.ova"; then
            same "ova: SMBIOS UUID (from the package's machine section)" "$(report uuid)" "$WANT"
            has "ova: first card's MAC" "$(report nics)" "08:00:27:1a:2b:3c/"
            same "ova: firmware" "$(report fw)" "bios"
        fi
        done_with ova; rm -f "$W/idvbox.ova"
    fi
else skp "vbox and ova" "VBoxManage isn't installed"; fi

echo "-- a config.pvs bundle: an .hds disk, SystemUuid, UEFI"
B=$W/p.pvm; mkdir -p "$B/harddisk.hdd"
graw; qemu-img convert -q -f raw -O parallels "$W/g.raw" "$B/harddisk.hdd/harddisk.hdd.0.{5fbaabe3-6958-40ff-92a7-860e329aab41}.hds"
SEC=$(( $(stat -c %s "$W/g.raw") / 512 ))
cat >"$B/harddisk.hdd/DiskDescriptor.xml" <<EOF
<?xml version='1.0' encoding='UTF-8'?>
<Parallels_disk_image Version="1.0">
 <Disk_Parameters><Disk_size>$SEC</Disk_size></Disk_Parameters>
 <StorageData><Storage><Start>0</Start><End>$SEC</End><Blocksize>2048</Blocksize>
  <Image><GUID>{5fbaabe3-6958-40ff-92a7-860e329aab41}</GUID><Type>Compressed</Type><File>harddisk.hdd.0.{5fbaabe3-6958-40ff-92a7-860e329aab41}.hds</File></Image>
 </Storage></StorageData>
 <Snapshots><Shot><GUID>{5fbaabe3-6958-40ff-92a7-860e329aab41}</GUID><ParentGUID>{00000000-0000-0000-0000-000000000000}</ParentGUID></Shot></Snapshots>
</Parallels_disk_image>
EOF
cat >"$B/config.pvs" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<ParallelsVirtualMachine schemaVersion="1.0">
 <Identification><VmUuid>{deadbeef-0000-4000-8000-00000000c0de}</VmUuid><VmName>Identity, pvs</VmName></Identification>
 <Settings><General><OsType>9</OsType></General><Startup><Bios><EfiEnabled>1</EfiEnabled></Bios></Startup></Settings>
 <Hardware>
  <Cpu><Number>2</Number></Cpu><Memory><RAM>1024</RAM></Memory>
  <Hdd id="0"><Enabled>1</Enabled><SystemName>/Users/someone/p.pvm/harddisk.hdd</SystemName><InterfaceType>2</InterfaceType></Hdd>
  <NetworkAdapter id="0"><Enabled>1</Enabled><MAC>001C42ABCDEF</MAC></NetworkAdapter>
 </Hardware>
 <SystemUuid>{feedface-1111-4222-8333-444455556666}</SystemUuid>
</ParallelsVirtualMachine>
EOF
rm -f "$W/g.raw"
if boot pvs "$B"; then
    same "pvs: SMBIOS UUID" "$(report uuid)" "feedface-1111-4222-8333-444455556666"
    has "pvs: card's MAC" "$(report nics)" "00:1c:42:ab:cd:ef/e1000e"
    same "pvs: firmware" "$(report fw)" "uefi"
fi
rm -rf "${B:?}"; done_with pvs

echo "-- a loose VHDX: a machine around it, with a new identity"
graw; qemu-img convert -q -f raw -O vhdx "$W/g.raw" "$W/loose.vhdx"; rm -f "$W/g.raw"
if boot vhdx "$W/loose.vhdx" --firmware bios --vcpus 1 --memory 1024; then
    t "vhdx: the guest booted from it, on SATA" '[ -n "$(report uuid)" ] && case "$(report disks)" in sda*) true;; *) false;; esac'
fi
rm -f "$W/loose.vhdx"; done_with vhdx
gate_end "vm-import-identity"
