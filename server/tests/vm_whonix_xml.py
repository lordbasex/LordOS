# vm_whonix_xml.py — run by tests/vm_whonix_xml_test.sh (python3 -I): Whonix's own libvirt XML for a member against the
# QEMU and passt command lines lord-vm logged for it (docs/VIRTUALIZATION.md 19.5, 19.10). Every element and attribute
# of the XML must be one this table knows: either mirrored (and the arguments are checked for it) or a deliberate
# difference with its reason. Anything new in a Whonix release (a device, a timer, a memory setting) fails the test, so a
# move of the catalog to that release is reviewed. Also checks what 19.5 adds or takes away beyond the XML.
#   python3 -I vm_whonix_xml.py ROLE XML LOGFILE   → "ok …" / "FAIL …" lines, exit 1 on any FAIL
import re, sys
import xml.etree.ElementTree as ET

role, xml_path, log_path = sys.argv[1], sys.argv[2], sys.argv[3]
fails = 0
def ok(s): print(f"ok: {role}: {s}")
def bad(s):
    global fails
    fails += 1
    print(f"FAIL: {role}: {s}")

log = open(log_path, encoding="utf-8", errors="replace").read()
# the QEMU line, logged in pieces ("qemu: " then "qemu+ " lines)
qm = []
for l in log.splitlines():
    m0 = re.search(r"qemu: (\S*qemu-system-x86_64 .*)", l)
    if m0: qm.append(m0.group(1)); continue
    m1 = re.search(r"qemu\+ (.*)", l)
    if m1 and qm: qm[-1] += m1.group(1)
pm = re.findall(r"passt: (\S*passt .*)", log)
if not qm: print(f"FAIL: {role}: no QEMU command line in {log_path}"); sys.exit(1)
q = qm[-1]
args = q.split(" ")
def opt(name):
    return [args[i + 1] for i in range(len(args) - 1) if args[i] == name]
devices, machine, cpu, glob = opt("-device"), ",".join(opt("-machine")), ",".join(opt("-cpu")), opt("-global")
has_dev = lambda pfx: any(d.split(",")[0] == pfx for d in devices)

root = ET.parse(xml_path).getroot()
# every path in the XML (element, then its attributes) must be listed here
KNOWN = {
    "domain", "domain@type", "domain/name", "domain/description", "domain/genid",
    "domain/memory", "domain/memory@dumpCore", "domain/memory@unit", "domain/currentMemory", "domain/currentMemory@unit",
    "domain/memoryBacking", "domain/memoryBacking/allocation", "domain/memoryBacking/allocation@mode", "domain/memoryBacking/discard",
    "domain/memoryBacking/nosharepages", "domain/vcpu", "domain/vcpu@placement", "domain/vcpu@cpuset", "domain/cpu", "domain/cpu@mode",
    "domain/os", "domain/os/type", "domain/os/type@machine", "domain/os/boot", "domain/os/boot@dev", "domain/os/bootmenu", "domain/os/bootmenu@enable",
    "domain/features", "domain/features/acpi", "domain/features/hap", "domain/features/pvspinlock", "domain/features/pvspinlock@state",
    "domain/features/pmu", "domain/features/pmu@state", "domain/features/vmport", "domain/features/vmport@state",
    "domain/clock", "domain/clock@offset", "domain/clock/timer", "domain/clock/timer@name", "domain/clock/timer@present",
    "domain/clock/timer@tickpolicy", "domain/clock/timer@track",
    "domain/on_poweroff", "domain/on_reboot", "domain/on_crash", "domain/pm", "domain/pm/suspend-to-mem", "domain/pm/suspend-to-mem@enabled",
    "domain/pm/suspend-to-disk", "domain/pm/suspend-to-disk@enabled",
    "domain/devices", "domain/devices/disk", "domain/devices/disk@type", "domain/devices/disk@device", "domain/devices/disk/driver",
    "domain/devices/disk/driver@name", "domain/devices/disk/driver@type", "domain/devices/disk/source", "domain/devices/disk/source@pool",
    "domain/devices/disk/source@volume", "domain/devices/disk/target", "domain/devices/disk/target@dev", "domain/devices/disk/target@bus",
    "domain/devices/interface", "domain/devices/interface@type", "domain/devices/interface/backend", "domain/devices/interface/backend@type",
    "domain/devices/interface/model", "domain/devices/interface/model@type", "domain/devices/interface/source", "domain/devices/interface/source@address",
    "domain/devices/interface/source@port", "domain/devices/interface/source/local", "domain/devices/interface/source/local@address",
    "domain/devices/interface/source/local@port",
    "domain/devices/controller", "domain/devices/controller@type", "domain/devices/controller@index",
    "domain/devices/serial", "domain/devices/serial@type", "domain/devices/serial/target", "domain/devices/serial/target@port",
    "domain/devices/console", "domain/devices/console@type", "domain/devices/console/target", "domain/devices/console/target@type",
    "domain/devices/console/target@port", "domain/devices/channel", "domain/devices/channel@type", "domain/devices/channel/target",
    "domain/devices/channel/target@type", "domain/devices/channel/target@name", "domain/devices/channel/address", "domain/devices/channel/address@type",
    "domain/devices/channel/address@controller", "domain/devices/channel/address@bus", "domain/devices/channel/address@port",
    "domain/devices/graphics", "domain/devices/graphics@type", "domain/devices/graphics@autoport", "domain/devices/graphics/listen",
    "domain/devices/graphics/listen@type", "domain/devices/graphics/clipboard", "domain/devices/graphics/clipboard@copypaste",
    "domain/devices/graphics/filetransfer", "domain/devices/graphics/filetransfer@enable", "domain/devices/graphics/gl", "domain/devices/graphics/gl@enable",
    "domain/devices/video", "domain/devices/video/model", "domain/devices/video/model@type", "domain/devices/video/model@heads",
    "domain/devices/video/model@primary", "domain/devices/video/model/acceleration", "domain/devices/video/model/acceleration@accel3d",
    "domain/devices/sound", "domain/devices/sound@model", "domain/devices/sound/codec", "domain/devices/sound/codec@type",
    "domain/devices/memballoon", "domain/devices/memballoon@model", "domain/devices/rng", "domain/devices/rng@model", "domain/devices/rng/rate",
    "domain/devices/rng/rate@bytes", "domain/devices/rng/rate@period", "domain/devices/rng/backend", "domain/devices/rng/backend@model",
}
def walk(e, path):
    p = f"{path}/{e.tag}" if path else e.tag
    yield p
    for a in e.attrib: yield f"{p}@{a}"
    for c in e: yield from walk(c, p)
unknown = sorted(set(x for x in walk(root, "") if x not in KNOWN))
if unknown: bad(f"Whonix's XML has what this test doesn't know (review 19.5 and this table): {', '.join(unknown)}")
else: ok("every element and attribute of Whonix's XML is known")

f = lambda path: root.find(path)
# memory and vCPUs
mem_gb = int(f("memory").text)
m = opt("-m")
(ok if m and int(m[0].split(",")[0]) == mem_gb * 1024 else bad)(f"memory {mem_gb} GB → -m {m}")
(ok if re.search(rf"memory-backend-memfd,id=mem,size={mem_gb * 1024}M,share=off", q) else bad)("guest memory: a private memfd (19.5: never hugetlb)")
(ok if "dump-guest-core=off" in machine else bad)("memory dumpCore='off' → dump-guest-core=off")
(ok if f("memoryBacking/nosharepages") is not None and "mem-merge=off" in machine else bad)("nosharepages → mem-merge=off (no KSM)")
smp = opt("-smp")
(ok if smp and smp[0].split(",")[0] == f("vcpu").text.strip() else bad)(f"vcpu {f('vcpu').text.strip()} → -smp {smp}")
(ok if cpu.startswith("host") else bad)("cpu host-passthrough → -cpu host")
(ok if "pmu=off" in cpu else bad)("pmu off → pmu=off")
(ok if "kvm-pv-unhalt=on" in cpu or f("features/pvspinlock") is None else bad)("pvspinlock on → kvm-pv-unhalt=on")
(ok if "vmport=off" in machine else bad)("vmport off → vmport=off")
(ok if "q35" in machine else bad)("machine q35")
(ok if f("genid") is not None and has_dev("vmgenid") else bad)("genid → vmgenid")
# timers (libvirt 11.3 with QEMU 10.0, 19.0)
timers = {t.get("name"): t for t in root.findall("clock/timer")}
kc = timers.get("kvmclock")
if kc is not None:
    want = "kvmclock=off" if kc.get("present") == "no" else None
    (ok if (want and want in cpu) or (not want and "kvmclock=off" not in cpu) else bad)(f"kvmclock present={kc.get('present')} → -cpu {cpu}")
rtc = opt("-rtc")
rt = timers.get("rtc")
if rt is not None and rt.get("tickpolicy") == "catchup":
    (ok if rtc and "driftfix=slew" in rtc[0] and "clock=vm" in rtc[0] else bad)(f"rtc catchup/track guest → -rtc {rtc}")
else:
    (ok if rtc and rtc[0] == "base=utc" else bad)(f"rtc → -rtc {rtc} (q35's RTC can't be removed: libvirt ignores present='no')")
(ok if "kvm-pit.lost_tick_policy=delay" in glob else bad)("pit → kvm-pit.lost_tick_policy=delay (libvirt's)")
(ok if "hpet=off" in machine else bad)("hpet present='no' → hpet=off")
(ok if "ICH9-LPC.disable_s3=1" in glob and "ICH9-LPC.disable_s4=1" in glob else bad)("pm suspend off → ICH9-LPC.disable_s3/s4")
# devices
(ok if any(d.startswith("virtio-blk-pci") for d in devices) else bad)("disk virtio → virtio-blk-pci")
(ok if has_dev("virtio-rng-pci") else bad)("rng virtio → virtio-rng-pci")
(ok if not has_dev("virtio-balloon-pci") else bad)("memballoon none → no balloon")
(ok if has_dev("virtio-vga") or has_dev("virtio-vga-gl") is False else bad)("video virtio, no 3D → virtio-vga")
(ok if not has_dev("virtio-vga-gl") and "gl=on" not in q else bad)("accel3d no → no virgl")
ifs = root.findall("devices/interface")
nics = [d for d in devices if d.startswith("virtio-net-pci")]
(ok if len(nics) == len(ifs) else bad)(f"{len(ifs)} interfaces → {len(nics)} virtio-net cards")
user = [i for i in ifs if i.get("type") == "user"]
if user:
    (ok if pm else bad)("interface user/passt → passt")
    p = pm[-1] if pm else ""
    (ok if "--dns-forward" not in p and "--no-dhcp-dns" in p and "--no-map-gw" in p else bad)(f"passt for Tor: no DNS forwarding, no host gateway ({p})")
    (ok if not re.search(r" -[tu] (?!none\b)", f" {p} ") else bad)("passt: no forwarded ports (-t none -u none)")
else:
    (ok if not pm else bad)("no passt for the workstation (its only card is the link)")
(ok if [i for i in ifs if i.get("type") == "udp"] and "hubid=" in q and "lnk0" in q else bad)("interface udp 127.0.0.1 → the private link's card (a socketpair, never UDP on loopback: 19.3)")
# deliberate differences: no agent channel, no SPICE, no serial for the guest's console beyond lord-vm's own
(ok if not has_dev("virtio-serial-pci") and "spicevmc" not in q and "-spice" not in args else bad)("virtio-serial + spicevmc channel → none (19.5: no host-to-guest channel)")
snd = root.find("devices/sound")
if role == "gateway":
    (ok if not has_dev("ich9-intel-hda") else bad)("sound ich9 → none on the gateway (19.5: it needs none)")
else:
    (ok if has_dev("ich9-intel-hda") and has_dev("hda-output") and not has_dev("hda-micro") and not has_dev("hda-duplex") else bad)("sound ich9 output codec → ich9-intel-hda + hda-output, no microphone")
(ok if snd is None or snd.find("codec").get("type") == "output" else bad)("the XML's codec is output-only")
# what 19.5 takes away beyond the XML
(ok if "-smbios" not in args else bad)("no -smbios strings")
(ok if "fw_cfg" not in q or "io.systemd.credentials" not in q else bad)("no first-boot credentials")
(ok if not any("serial=" in d for d in devices if d.startswith("virtio-blk")) else bad)("no disk serial")
(ok if "hugetlb" not in q and "memory-backend-file" not in q else bad)("no huge pages")
(ok if not has_dev("usb-tablet") and not has_dev("qemu-xhci") else bad)("no USB")
sys.exit(1 if fails else 0)
