# stop3d.py — a real 3D guest's stop (docs/VIRTUALIZATION.md 3.2 "Graphics 3D", QEMU issue 4918): the harness's Debian 13
# guest (images.py) with 3D (virgl) on a render node of this computer, its kernel brought to Debian's current one (6.12.117
# or later aborts QEMU 10.0.13 at each guest shutdown or restart), started by this build's lord-vm in a scratch library
# (private namespaces, like boot-matrix.py). Then, with lord-vm's own reading of each end (run.json's reason):
#   1. no guest tools: the guest powers itself off after 30 s of running   → stopped, qemu-3d-stop (or guest-shutdown
#                                                                             when QEMU didn't abort)
#   2. this build's lordos-guest and its systemd-shutdown hook installed:
#      a restart after 30 s                                                 → stopped, reset
#      a power-off after 30 s                                               → stopped, guest-shutdown
# Test only: nothing here ships.   python3 -I tools/vm/e2e/machines/stop3d.py [--render-node /dev/dri/renderDN]
import base64, json, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mh  # noqa: E402
from targets import Lib  # noqa: E402

AGENT = os.path.join(mh.BUILD, "lordos-guest")


def end_of(L, M, timeout=120):
    end = time.time() + timeout
    while time.time() < end:
        st = L.status(M)
        if st.get("state") in ("stopped", "crashed"):
            return st
        time.sleep(0.5)
    return L.status(M)


def lines(M, pat):
    t = open(os.path.join(M, "log", "lord-vm.log"), errors="replace").read()
    return [l[:220] for l in t.splitlines() if re.search(pat, l)]


def main():
    a = sys.argv[1:]
    node = a[a.index("--render-node") + 1] if "--render-node" in a else next(
        ("/dev/dri/" + n for n in sorted(os.listdir("/dev/dri")) if n.startswith("renderD")), None) if os.path.isdir("/dev/dri") else None
    if not mh.kvm_ok() or not node:
        print("skipped: needs KVM and a render node"); return 77
    r = mh.Results("a 3D guest's stop (%s)" % node)
    L = Lib()
    try:
        M = L.make("stop3d", "--accel", "kvm", "--graphics", "3d", "--vcpus", "2", "--memory", "4096")
        L.boot(M, "--render-node", node)
        r.t("3D is on (virtio-vga-gl)", bool(lines(M, r"virtio-vga-gl")))
        con = L.console(M)
        k0, _ = con.run("uname -r", 20)
        o, st = con.run("DEBIAN_FRONTEND=noninteractive apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "
                        "linux-image-amd64 >/tmp/k.log 2>&1; echo K=$?; ls /boot/vmlinuz-* | sort -V | tail -1", 1500)
        print("kernel was %s; %s" % (k0.strip(), o.strip()[-200:]), flush=True)
        con.run("systemctl reboot", 5)
        con.close()
        # the reboot runs the old kernel's shutdown: QEMU may abort here too (that kernel may already be one of them)
        st = end_of(L, M, 90) if L.status(M).get("state") != "running" else None
        time.sleep(5)
        if L.status(M).get("state") != "running":
            print("after the kernel update's reboot: %s %s" % (L.status(M).get("state"), L.status(M).get("reason")), flush=True)
            L.boot(M, "--render-node", node)
        else:
            end = time.time() + 180
            serial = os.path.join(M, "log", "serial.log")
            while time.time() < end and open(serial, errors="replace").read().count("MH-READY") < 2:
                time.sleep(1)
        con = L.console(M)
        k1, _ = con.run("uname -r", 20)
        con.close()
        k1 = k1.strip().splitlines()[-1] if k1.strip() else "?"
        m = re.match(r"(\d+)\.(\d+)\.(\d+)", k1)
        new = bool(m) and tuple(map(int, m.groups())) >= (6, 12, 117)
        r.t("the guest runs Debian's current kernel, 6.12.117 or later (%s)" % k1, new)
        # 1. no tools: power off after 30 s of running
        time.sleep(35)
        con = L.console(M)
        con.run("systemctl poweroff", 5)
        con.close()
        st = end_of(L, M)
        print("1. no tools, power off: %s %s code %s" % (st.get("state"), st.get("reason"), st.get("code")), flush=True)
        for l in lines(M, r"3D|abort|qemu: exited|state stopped|state crashed")[-6:]:
            print("   | " + l)
        r.t("1. no tools: stopped, qemu-3d-stop or guest-shutdown (%s)" % st.get("reason"),
            st.get("state") == "stopped" and st.get("reason") in ("qemu-3d-stop", "guest-shutdown"))
        # 2. this build's lordos-guest and its hook
        L.boot(M, "--render-node", node)
        con = L.console(M)
        b64 = base64.b64encode(open(AGENT, "rb").read()).decode()
        con.run("rm -f /tmp/lg.b64", 10)
        for i in range(0, len(b64), 3000):
            con.run("printf %%s '%s' >> /tmp/lg.b64" % b64[i:i + 3000], 20)
        o, _ = con.run("base64 -d /tmp/lg.b64 > /usr/local/bin/lordos-guest && chmod 755 /usr/local/bin/lordos-guest && "
                       "install -d -m 755 /usr/lib/systemd/system-shutdown && printf '%s\\n' '#!/bin/sh' "
                       "'exec /usr/local/bin/lordos-guest --stopping \"$1\"' > /usr/lib/systemd/system-shutdown/lordos-guest-local && "
                       "chmod 755 /usr/lib/systemd/system-shutdown/lordos-guest-local && sha256sum /usr/local/bin/lordos-guest", 60)
        import hashlib
        r.t("lordos-guest and its hook are in the guest", hashlib.sha256(open(AGENT, "rb").read()).hexdigest() in o)
        time.sleep(35)
        con.run("systemctl reboot", 5)
        con.close()
        st = end_of(L, M)
        print("2a. tools, restart: %s %s code %s" % (st.get("state"), st.get("reason"), st.get("code")), flush=True)
        for l in lines(M, r"3D|abort|stopping|qemu: exited|state stopped|state crashed")[-6:]:
            print("   | " + l)
        r.t("2a. tools, restart: stopped, reset (%s)" % st.get("reason"), st.get("state") == "stopped" and st.get("reason") == "reset")
        L.boot(M, "--render-node", node)
        time.sleep(35)
        con = L.console(M)
        con.run("systemctl poweroff", 5)
        con.close()
        st = end_of(L, M)
        print("2b. tools, power off: %s %s code %s" % (st.get("state"), st.get("reason"), st.get("code")), flush=True)
        for l in lines(M, r"3D|abort|stopping|qemu: exited|state stopped|state crashed|qemu-img check")[-6:]:
            print("   | " + l)
        r.t("2b. tools, power off: stopped, guest-shutdown (%s)" % st.get("reason"), st.get("state") == "stopped" and st.get("reason") == "guest-shutdown")
        # the next start checks the disks (QEMU didn't close them)
        L.boot(M, "--render-node", node)
        chk = lines(M, r"qemu-img check|leaks|disk check")
        print("disk check lines: %s" % chk[-3:])
        L.off(M)
    finally:
        L.close()
    return r.end()


if __name__ == "__main__":
    mh.main_guard(main)
