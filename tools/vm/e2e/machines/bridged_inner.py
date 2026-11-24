# bridged_inner.py — run by nolandlock.py (--inner bridged_inner.py --keep-landlock --as root --image) inside its Debian 13
# guest, which is the computer here, so that no card of a real computer is touched: a bridged machine (docs/
# VIRTUALIZATION.md 6.5) under QEMU's seccomp allowlist (3.3). The macvtap is made by hand the way the admin helper's
# vm-net-bridge makes it (vmnet_admin.c: named after the machine, on the wired card, the machine's MAC, its /dev/tapN
# given to the person); this tree's lord-vm checks and opens it and QEMU gets the descriptor. The machine is the harness's
# guest image again (nested KVM), started as the guest's person. Checked: lord-vm's "bridged" line, QEMU's sandbox line
# (allowlist=1), packets through the macvtap both ways, no call stopped by the allowlist. Prints PASS/FAIL lines.
import json, os, re, subprocess, sys, time
sys.path.insert(0, "/mnt/src/tools/vm/e2e/machines")
import mh

r = mh.Results("a bridged machine under the allowlist (nested)")
VM = "/mnt/src/server/build/lord-vm"
LIB = "/home/tester/Machines"
IMG = "/mnt/src/.mh-image.qcow2"


def sh(cmd, t=600):
    p = subprocess.run(["sh", "-c", cmd], capture_output=True, text=True, timeout=t)
    return p.returncode, (p.stdout + p.stderr)


def vm(*a, t=300):
    env = "HOME=/home/tester XDG_RUNTIME_DIR=/tmp/xdg-tester LANG=C.UTF-8 PATH=/usr/bin:/bin:/usr/sbin:/sbin"
    return sh("runuser -u tester -- env %s %s %s" % (env, VM, " ".join("'%s'" % x for x in a)), t)


sh("install -d -o tester -g tester -m 700 /tmp/xdg-tester %s; chmod 666 /dev/kvm" % LIB, 30)
card = sh("ip -o route show default | awk '{print $5; exit}'", 10)[1].strip()
r.t("the guest's wired card: %s" % card, bool(card))
sh("cp %s /home/tester/img.qcow2 && chown tester: /home/tester/img.qcow2" % IMG, 300)
M = LIB + "/Bridged.lordvm"
rc, o = vm("--create", M, "--name", "Bridged", "--accel", "kvm", "--memory", "1024", "--vcpus", "1", "--display", "none",
           "--import", "/home/tester/img.qcow2")
r.t("made", rc == 0, o[-200:])
rc, o = vm("--set", M, "--network", "bridged", "--bridge", card)
r.t("--network bridged --bridge %s" % card, rc == 0, o[-200:])
mj = json.load(open(M + "/machine.json"))
mid, mac = mj["id"], mj["mac"]
name = "lvm%s%s" % (mid[:8], mid[9:13])
rc, o = sh("ip link add link %s name %s address %s type macvtap mode bridge && ip link set %s up && "
           "idx=$(cat /sys/class/net/%s/ifindex) && chown tester /dev/tap$idx && ls -la /dev/tap$idx" % (card, name, mac, name, name), 30)
r.t("the macvtap %s, its device given to the person" % name, rc == 0, o)
print("   " + o.strip())
rc, o = vm("--start", M, "--detach", "--keep-running")
serial = M + "/log/serial.log"
end = time.time() + 240
while time.time() < end and "MH-READY" not in (open(serial, errors="replace").read() if os.path.exists(serial) else ""):
    time.sleep(1)
log = open(M + "/log/lord-vm.log", errors="replace").read()
r.t("the guest booted on the bridged card", "MH-READY" in open(serial, errors="replace").read(), "\n".join(log.splitlines()[-8:]))
bl = [l for l in log.splitlines() if "bridged on" in l]
sb = [l for l in log.splitlines() if "sandbox:" in l]
print("   | " + (bl[-1] if bl else "no bridged line"))
print("   | " + (sb[-1][:220] if sb else "no sandbox line"))
r.t("lord-vm opened the macvtap", bool(bl))
r.t("QEMU under our allowlist (allowlist=1)", bool(sb) and "allowlist=1" in sb[-1])
time.sleep(20)                               # the guest's DHCP asks go out through the macvtap
rc, o = sh("ip -s link show %s" % name, 10)
m = re.findall(r"(?:RX|TX):.*\n\s+(\d+)\s+(\d+)", o)
print("   " + o.strip().replace("\n", "\n   "))
r.t("packets through the macvtap (RX bytes/packets, TX bytes/packets: %s)" % m, len(m) == 2 and int(m[0][1]) > 0 and int(m[1][1]) > 0)
log = open(M + "/log/lord-vm.log", errors="replace").read()
stopped = [l for l in log.splitlines() if re.search(r"seccomp.*(stopp|refus|denied)|SIGSYS|not allowed", l)]
r.t("no call stopped by the allowlist", not stopped and "state running" in log, "; ".join(stopped[:3]))
vm("--poweroff", M)
sh("ip link del %s" % name, 10)
sys.exit(r.end())
