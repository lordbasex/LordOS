# hugetlb.py — a machine on 2 MB hugetlb pages (docs/VIRTUALIZATION.md 3.7.2) run under QEMU's seccomp allowlist (3.3):
# the administrator's reservation made for the run (sudo sysctl vm.nr_hugepages, put back afterwards), the harness's Debian
# 13 guest (images.py) started by this build's lord-vm in a scratch library (private namespaces), booted to MH-READY;
# lord-vm's memory line, the pages taken, no call stopped by the allowlist. Test only; needs `sudo -n`.
#   python3 -I tools/vm/e2e/machines/hugetlb.py
import os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mh  # noqa: E402
from targets import Lib  # noqa: E402


def meminfo(k):
    return int(re.search(r"^%s:\s+(\d+)" % k, open("/proc/meminfo").read(), re.M).group(1))


def main():
    if not mh.kvm_ok() or subprocess.run(["sudo", "-n", "true"], capture_output=True).returncode:
        print("skipped: needs KVM and sudo -n"); return 77
    r = mh.Results("hugetlb memory under the allowlist")
    was = int(open("/proc/sys/vm/nr_hugepages").read())
    L = Lib()
    try:
        subprocess.run(["sudo", "-n", "sysctl", "-q", "vm.nr_hugepages=%d" % (was + 600)], check=True)
        free0 = meminfo("HugePages_Free")
        r.t("600 pages reserved for the run (%d free)" % free0, free0 >= 600)
        M = L.make("huge", "--accel", "kvm", "--vcpus", "2", "--memory", "1024")
        t = L.boot(M)
        log = open(os.path.join(M, "log", "lord-vm.log"), errors="replace").read()
        ml = [l for l in log.splitlines() if "memory:" in l]
        print("   | " + "\n   | ".join(ml[-2:]))
        r.t("lord-vm chose hugetlb pages", any("hugetlb pages" in l for l in ml))
        sb = [l for l in log.splitlines() if "sandbox:" in l]
        print("   | " + (sb[-1][:200] if sb else ""))
        r.t("QEMU runs under our allowlist (allowlist=1)", bool(sb) and "allowlist=1" in sb[-1])
        free1 = meminfo("HugePages_Free")
        r.t("the guest booted on them in %.1f s; %d pages taken" % (t, free0 - free1), free0 - free1 >= 500)
        con = L.console(M)
        o, st = con.run("head -c 700M /dev/urandom | sha256sum >/dev/null; free -m | sed -n 2p", 120)
        con.close()
        print("   guest:", o.strip()[-100:])
        r.t("the guest uses its memory (700 MB through a pipe)", "Mem:" in o)
        stopped = [l for l in open(os.path.join(M, "log", "lord-vm.log"), errors="replace").read().splitlines()
                   if re.search(r"seccomp.*(stopp|refus|denied)|SIGSYS", l)]
        r.t("no call stopped by the allowlist", not stopped, "; ".join(stopped[:3]))
        L.off(M)
        time.sleep(2)
        r.t("the pages came back at power-off", meminfo("HugePages_Free") == free0)
    finally:
        L.close()
        subprocess.run(["sudo", "-n", "sysctl", "-q", "vm.nr_hugepages=%d" % was])
        print("vm.nr_hugepages back to", open("/proc/sys/vm/nr_hugepages").read().strip())
    return r.end()


if __name__ == "__main__":
    mh.main_guard(main)
