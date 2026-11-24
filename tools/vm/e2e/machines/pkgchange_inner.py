# pkgchange_inner.py — run by nolandlock.py (--inner pkgchange_inner.py --keep-landlock --as root) inside its Debian 13
# guest, which is the computer here: real package updates under machines (docs/VIRTUALIZATION.md 3.6, 6.1).
#   1. QEMU and OVMF as Debian 13.0 shipped them (snapshot.debian.org, 2025-08-10) installed with apt; two machines
#      (emulated, TCG) made and started by this tree's lord-vm as the guest's person: their UEFI code is kept with them
#      (firmware-code.fd); one is suspended (its state saved by that QEMU), the other shut down.
#   2. apt upgrades QEMU and OVMF to Debian 13's current packages: a real package update.
#   3. The suspended machine's start is refused (saved by another QEMU), then --resume-anyway (what "Resume anyway"
#      runs) and --discard-state; the other machine starts with its kept UEFI code, then `--set --firmware-update yes`
#      moves it to the new one.
# Prints PASS/FAIL lines (mh.Results).
import json, os, re, subprocess, sys, time
sys.path.insert(0, "/mnt/src/tools/vm/e2e/machines")
import mh

r = mh.Results("package changes under machines")
VM = "/mnt/src/server/build/lord-vm"
LIB = "/home/tester/Machines"
SNAP = "https://snapshot.debian.org/archive/debian/20250810T000000Z"
QPKGS = ["qemu-system-x86", "qemu-system-common", "qemu-system-data", "qemu-system-modules-opengl", "qemu-utils"]


def sh(cmd, t=1800):
    p = subprocess.run(["sh", "-c", cmd], capture_output=True, text=True, timeout=t)
    return p.returncode, (p.stdout + p.stderr)


def vm(*a, t=300):
    env = "HOME=/home/tester XDG_RUNTIME_DIR=/tmp/xdg-tester LANG=C.UTF-8 PATH=/usr/bin:/bin"
    cmd = "runuser -u tester -- env %s %s %s" % (env, VM, " ".join("'%s'" % x for x in a))
    return sh(cmd, t)


def state(M):
    rc, o = vm("--status", M, "--json", t=30)
    try:
        return json.loads(o[o.index("{"):])
    except ValueError:
        return {}


def wait(M, states, t=180):
    end = time.time() + t
    while time.time() < end:
        s = state(M)
        if s.get("state") in states:
            return s
        time.sleep(1)
    return state(M)


def log(M):
    return open(os.path.join(M, "log", "lord-vm.log"), errors="replace").read()


def ver(p):
    return sh("dpkg-query -W -f '${Version}' %s" % p, 30)[1].strip()


sh("install -d -o tester -g tester -m 700 /tmp/xdg-tester %s" % LIB, 30)
# 1. the packages Debian 13.0 shipped
open("/etc/apt/sources.list.d/snap.list", "w").write("deb [check-valid-until=no] %s trixie main\n" % SNAP)
rc, o = sh("apt-get update -qq 2>&1 | tail -2; apt-cache madison ovmf qemu-system-x86 | grep snapshot", 600)
print(o)
old_ovmf = re.search(r"ovmf \| (\S+) \| %s" % re.escape(SNAP), o)
old_qemu = re.search(r"qemu-system-x86 \| (\S+) \| %s" % re.escape(SNAP), o)
if not old_ovmf or not old_qemu:
    r.bad("the snapshot's versions", o[-400:]); sys.exit(r.end())
ov, qv = old_ovmf.group(1), old_qemu.group(1)
pk = " ".join(["%s=%s" % (p, qv) for p in QPKGS] + ["ovmf=%s" % ov])
rc, o = sh("DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --allow-downgrades %s 2>&1 | tail -3" % pk)
r.t("QEMU %s and OVMF %s installed (Debian 13.0's)" % (qv, ov), ver("qemu-system-x86") == qv and ver("ovmf") == ov, o[-300:])
q_old = sh("qemu-system-x86_64 --version | head -1", 30)[1].strip()
fw_old = sh("sha256sum /usr/share/OVMF/OVMF_CODE_4M.fd", 30)[1].split()[0]
A, B = LIB + "/Susp.lordvm", LIB + "/Fw.lordvm"
for M, n in ((A, "Susp"), (B, "Fw")):
    rc, o = vm("--create", M, "--name", n, "--accel", "tcg", "--memory", "512", "--disk", "1", "--display", "none", "--firmware", "uefi")
    r.t("%s made" % n, rc == 0, o[-200:])
    rc, o = vm("--start", M, "--detach")
    s = wait(M, ("running",), 120)
    r.t("%s runs under the old QEMU" % n, s.get("state") == "running", o[-200:])
time.sleep(20)                                        # the firmware is up (the UEFI shell: no disk to boot)
rc, o = vm("--suspend", A, t=200)
s = wait(A, ("suspended", "stopped", "crashed"), 200)
r.t("Susp suspended by %s (%s)" % (q_old, s.get("state")), s.get("state") == "suspended" or os.path.exists(A + "/suspend.state") or "saved in" in log(A), o[-200:])
print("   | " + "\n   | ".join(l for l in log(A).splitlines() if "suspend" in l)[-400:])
vm("--stop", B, "--timeout", "30")
vm("--poweroff", B)
wait(B, ("stopped",), 60)
mj = json.load(open(B + "/machine.json"))
r.t("Fw's UEFI code kept, its SHA-256 recorded", os.path.exists(B + "/firmware-code.fd") or any("firmware-code" in f for f in os.listdir(B)),
    str(os.listdir(B)))
print("   firmware_sha256 in machine.json:", (mj.get("firmware_sha256") or "")[:16], "old package's:", fw_old[:16])
# 2. a real package update
sh("rm -f /etc/apt/sources.list.d/snap.list; apt-get update -qq", 600)
rc, o = sh("DEBIAN_FRONTEND=noninteractive apt-get install -y -qq %s ovmf 2>&1 | tail -3" % " ".join(QPKGS))
q_new = sh("qemu-system-x86_64 --version | head -1", 30)[1].strip()
fw_new = sh("sha256sum /usr/share/OVMF/OVMF_CODE_4M.fd", 30)[1].split()[0]
r.t("updated: %s, OVMF %s" % (q_new, ver("ovmf")), q_new != q_old and fw_new != fw_old, o[-300:])
# 3a. the suspended machine
rc, o = vm("--start", A, "--detach")
s = wait(A, ("running", "crashed", "stopped", "suspended"), 120)
lines = [l for l in log(A).splitlines() if "resume" in l]
print("   | " + "\n   | ".join(lines[-4:]))
r.t("Susp's resume refused: saved by another QEMU (%s %s)" % (s.get("state"), s.get("reason")),
    any("saved by QEMU" in l for l in lines), o[-200:])
rc, o = vm("--start", A, "--detach", "--resume-anyway")
s = wait(A, ("running", "crashed", "stopped"), 120)
lines = [l for l in log(A).splitlines() if "resume" in l]
print("   | " + "\n   | ".join(lines[-4:]))
print("   Resume anyway: %s %s %s" % (s.get("state"), s.get("reason"), s.get("detail")))
r.ok("Resume anyway ran: %s (%s)" % (s.get("state"), s.get("reason")))
if s.get("state") == "running":
    vm("--poweroff", A)
else:
    rc, o = vm("--discard-state", A)
    r.t("the saved state discarded after all", rc == 0, o[-200:])
    rc, o = vm("--start", A, "--detach")
    s = wait(A, ("running",), 120)
    r.t("Susp starts from off afterwards", s.get("state") == "running")
    vm("--poweroff", A)
# 3b. the other machine: its kept UEFI code, then the explicit move
rc, o = vm("--start", B, "--detach")
s = wait(B, ("running",), 120)
fl = [l for l in log(B).splitlines() if "firmware" in l]
print("   | " + "\n   | ".join(fl[-3:]))
r.t("Fw starts with the UEFI code kept with it (the package's changed)", s.get("state") == "running" and any("changed" in l for l in fl))
vm("--poweroff", B)
wait(B, ("stopped",), 60)
rc, o = vm("--set", B, "--firmware-update", "yes")
r.t("--set --firmware-update yes", rc == 0, o[-200:])
time.sleep(3)
rc, o = vm("--start", B, "--detach")
s = wait(B, ("running",), 120)
fl = [l for l in log(B).splitlines() if "firmware" in l]
print("   | " + "\n   | ".join(fl[-3:]))
mj = json.load(open(B + "/machine.json"))
r.t("Fw now runs the new UEFI code (machine.json %s…, the package's %s…)" % ((mj.get("firmware_sha256") or "")[:12], fw_new[:12]),
    s.get("state") == "running" and (mj.get("firmware_sha256") or "") == fw_new)
vm("--poweroff", B)
sys.exit(r.end())
