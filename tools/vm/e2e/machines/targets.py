# targets.py — the performance targets of docs/VIRTUALIZATION.md 9, measured on the computer it runs on: the harness's
# Debian 13 guest (images.py) made and started by this build's lord-vm in a scratch library, in private user and mount
# namespaces (mh.py), like boot-matrix.py. Test only: nothing here ships.
#
#   python3 -I tools/vm/e2e/machines/targets.py [--parts boot,cpu,disk,net,pause,suspend,idle] [--json FILE] [--vcpus N]
#
# Parts (each prints its figures; --json writes them all):
#   boot     seconds from `lord-vm --start` to the guest's "MH-READY" on its serial console, UEFI and SeaBIOS (amd64), 3 runs
#   cpu      SHA-256 of a 1 GiB file in memory (tmpfs) and of 4 GiB of zeros through a pipe (coreutils), and a Python
#            loop, best of 3, in the guest and on this computer
#   disk     dd of 2 GiB with direct I/O, write then read, in the guest (its qcow2 disk, virtio-blk, the iothread) and on
#            this computer in the same folder as the library (best of 3)
#   net      iperf3 over the Shared network (passt), 10 s each way, through a port forwarded to the guest
#            (this computer's iperf3: IPERF3=path, LD_LIBRARY_PATH as it needs)
#   pause    `lord-vm --pause` and `--resume` (the command's whole time, QMP stop/cont included), 10 times
#   suspend  an 8 GB machine with 7 GiB of its memory filled from /dev/urandom: lord-vm's own "suspend: N MB saved in
#            N ms" and "resume: loaded in N ms", and the whole time to a console that answers again
#   idle     host CPU of the idle guest's QEMU over 60 s, 90 s after its boot (no window: no lord-vmdisplay here)
# A part that can't run here prints why and is skipped.
import json, os, re, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mh  # noqa: E402
import images  # noqa: E402

VM = os.path.join(mh.BUILD, "lord-vm")
HZ = os.sysconf("SC_CLK_TCK")
PORT = int(os.environ.get("MH_IPERF_PORT", "15201"))


def median(v):
    v = sorted(v)
    return v[len(v) // 2] if len(v) % 2 else (v[len(v) // 2 - 1] + v[len(v) // 2]) / 2


class Lib:
    """A scratch library in private namespaces and the lord-vm calls on it."""

    def __init__(self):
        self.D = tempfile.mkdtemp(prefix="targets.", dir=mh.scratch())
        os.chmod(self.D, 0o700)
        for d in ("home/Machines", "run", "fake"):
            os.makedirs(os.path.join(self.D, d))
        self.env = {"PATH": "/usr/bin:/bin", "HOME": os.path.join(self.D, "home"), "XDG_RUNTIME_DIR": os.path.join(self.D, "run"),
                    "LANG": "C.UTF-8"}

    def vm(self, *args, timeout=600):
        return subprocess.run(mh.ns_prefix(self.D) + [VM] + list(args), cwd=mh.SERVER, env=self.env, capture_output=True, text=True,
                              timeout=timeout)

    def status(self, M):
        try:
            return json.loads(self.vm("--status", M, "--json", timeout=30).stdout or "{}")
        except ValueError:
            return {}

    def make(self, name, *create):
        M = os.path.join(self.D, "home", "Machines", name + ".lordvm")
        r = self.vm("--create", M, "--name", name, "--agent", "on", "--display", "window", "--import", images.agent_image(images.ARCH),
                    *create)
        if r.returncode:
            raise RuntimeError("create: " + (r.stdout + r.stderr)[-400:])
        return M

    def boot(self, M, *start, timeout=240):
        """--start, then the seconds to MH-READY (serial.log starts over at each start)."""
        t0 = time.time()
        r = self.vm("--start", M, "--detach", "--keep-running", *start)
        if r.returncode:
            raise RuntimeError("start: " + (r.stdout + r.stderr)[-400:])
        serial = os.path.join(M, "log", "serial.log")
        while time.time() - t0 < timeout:
            t = open(serial, errors="replace").read() if os.path.exists(serial) else ""
            if "MH-READY" in t:
                return time.time() - t0
            time.sleep(0.2)
        raise TimeoutError("no MH-READY after %ss" % timeout)

    def mid(self, M):
        return json.load(open(os.path.join(M, "machine.json")))["id"]

    def console(self, M):
        return mh.Console(self.env["XDG_RUNTIME_DIR"], self.mid(M), 30)

    def off(self, M):
        self.vm("--poweroff", M, timeout=60)

    def close(self):
        for m in os.listdir(os.path.join(self.D, "home", "Machines")):
            self.off(os.path.join(self.D, "home", "Machines", m))
        if os.environ.get("MH_KEEP") != "1":
            shutil.rmtree(self.D, ignore_errors=True)


def sh(cmd, timeout=600):
    return subprocess.run(["sh", "-c", cmd], capture_output=True, text=True, timeout=timeout)


def timed_host(cmd, n=3):
    best = None
    for _ in range(n):
        t0 = time.time()
        sh(cmd)
        d = time.time() - t0
        best = d if best is None else min(best, d)
    return best


def timed_guest(con, cmd, n=3, timeout=600):
    best = None
    for _ in range(n):
        out, st = con.run("s=$(date +%%s.%%N); %s >/dev/null 2>&1; e=$(date +%%s.%%N); awk \"BEGIN{print \\\"T=\\\" $e-$s}\"" % cmd, timeout=timeout)
        m = re.search(r"T=([0-9.]+)", out)
        if not m:
            raise RuntimeError("no time from the guest: %r" % out[-300:])
        d = float(m.group(1))
        best = d if best is None else min(best, d)
    return best


def dd_rate(text):
    m = re.search(r"([0-9.,]+) ([GMk]B)/s", text)
    if not m:
        return None
    v = float(m.group(1).replace(",", "."))
    return v * {"GB": 1000, "MB": 1, "kB": 0.001}[m.group(2)]


def proc_cpu(pid):
    f = open("/proc/%d/stat" % pid).read().rsplit(")", 1)[1].split()
    return (int(f[11]) + int(f[12])) / HZ


def main():
    a = sys.argv[1:]
    parts = (a[a.index("--parts") + 1] if "--parts" in a else "boot,cpu,disk,net,pause,suspend,idle").split(",")
    jout = a[a.index("--json") + 1] if "--json" in a else None
    vcpus = a[a.index("--vcpus") + 1] if "--vcpus" in a else "2"
    if not mh.kvm_ok():
        print("skipped: no /dev/kvm"); return 77
    if not os.access(VM, os.X_OK):
        print("skipped: needs server/build/lord-vm"); return 77
    host = {"arch": images.ARCH, "kernel": os.uname().release, "cpu": "", "loadavg": open("/proc/loadavg").read().split()[:3],
            "qemu": sh("qemu-system-%s --version" % ("x86_64" if images.ARCH == "amd64" else "aarch64")).stdout.split("\n")[0]}
    try:
        host["cpu"] = re.search(r"model name\s*:\s*(.*)", open("/proc/cpuinfo").read()).group(1)
    except (OSError, AttributeError):
        host["cpu"] = os.uname().machine
    res = {"host": host, "date": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "vcpus": int(vcpus)}
    L = Lib()
    try:
        if "boot" in parts:
            b = {}
            for fw in (["uefi", "bios"] if images.ARCH == "amd64" else ["uefi"]):
                M = L.make("boot-" + fw, "--accel", "kvm", "--firmware", fw, "--vcpus", vcpus, "--memory", "2048")
                v = []
                for _ in range(3):
                    v.append(round(L.boot(M), 1))
                    L.vm("--stop", M, "--timeout", "60", timeout=120)
                    end = time.time() + 90
                    while time.time() < end and L.status(M).get("state") not in ("stopped", "crashed"):
                        time.sleep(0.5)
                b[fw] = v
                print("boot %-4s to MH-READY: %s s" % (fw, v), flush=True)
                shutil.rmtree(M, ignore_errors=True)
            res["boot"] = b
        need_guest = [p for p in parts if p in ("cpu", "disk", "net", "pause", "idle")]
        if need_guest:
            M = L.make("targets", "--accel", "kvm", "--vcpus", vcpus, "--memory", "4096")
            r = L.vm("--set", M, "--port", "tcp:%d:5201:this-computer" % PORT)
            if r.returncode:
                raise RuntimeError("set: " + (r.stdout + r.stderr)[-400:])
            res["boot_main_s"] = round(L.boot(M), 1)
            st = json.load(open(os.path.join(M, "run.json")))
            qpid = int(st.get("qemu_pid") or 0)
            con = L.console(M)
            if "idle" in parts:
                time.sleep(90)
                c0, t0 = proc_cpu(qpid), time.time()
                time.sleep(60)
                c1, t1 = proc_cpu(qpid), time.time()
                res["idle_qemu_pct_of_core"] = round(100 * (c1 - c0) / (t1 - t0), 2)
                print("idle: QEMU %.2f %% of a core (60 s, 90 s after boot)" % res["idle_qemu_pct_of_core"], flush=True)
            if "cpu" in parts:
                # SHA-256 of a 1 GiB file in memory (tmpfs: no disk, no pipe), and of 4 GiB of zeros through a pipe
                con.run("mkdir -p /mnt/t && mount -t tmpfs -o size=1200m tmpfs /mnt/t && head -c 1G /dev/urandom > /mnt/t/f", 120)
                g0 = timed_guest(con, "sha256sum /mnt/t/f")
                hf = "/dev/shm/lordos-targets-%d" % os.getpid()
                sh("head -c 1G /dev/urandom > %s" % hf)
                h0 = timed_host("sha256sum %s" % hf)
                os.unlink(hf)
                con.run("rm -f /mnt/t/f", 30)
                g1 = timed_guest(con, "head -c 4G /dev/zero | sha256sum")
                h1 = timed_host("head -c 4G /dev/zero | sha256sum")
                out, _ = con.run("command -v python3 || (apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq python3-minimal) >/dev/null 2>&1; command -v python3", 600)
                py = "python3 -c 'sum(i*i for i in range(30000000))'"
                g2 = timed_guest(con, py) if "python3" in out else None
                h2 = timed_host(py)
                res["cpu"] = {"sha256_file_1g": {"guest_s": round(g0, 2), "host_s": round(h0, 2), "pct": round(100 * h0 / g0, 1)},
                              "sha256_pipe_4g": {"guest_s": round(g1, 2), "host_s": round(h1, 2), "pct": round(100 * h1 / g1, 1)},
                              "python": {"guest_s": round(g2, 2) if g2 else None, "host_s": round(h2, 2), "pct": round(100 * h2 / g2, 1) if g2 else None}}
                print("cpu: %s" % res["cpu"], flush=True)
            if "disk" in parts:
                gw, gr, hw, hr = [], [], [], []
                hf = os.path.join(L.D, "dd.host")
                for _ in range(3):
                    o, _ = con.run("dd if=/dev/zero of=/root/dd.t bs=1M count=2048 oflag=direct 2>&1 | tail -1", 300)
                    gw.append(dd_rate(o))
                    o, _ = con.run("echo 3 >/proc/sys/vm/drop_caches; dd if=/root/dd.t of=/dev/null bs=1M iflag=direct 2>&1 | tail -1", 300)
                    gr.append(dd_rate(o))
                    con.run("rm -f /root/dd.t; sync", 60)
                    hw.append(dd_rate(sh("LANG=C dd if=/dev/zero of=%s bs=1M count=2048 oflag=direct 2>&1 | tail -1" % hf).stdout))
                    hr.append(dd_rate(sh("LANG=C dd if=%s of=/dev/null bs=1M iflag=direct 2>&1 | tail -1" % hf).stdout))
                    os.unlink(hf)
                g = {"write": max(x for x in gw if x), "read": max(x for x in gr if x)}
                h = {"write": max(x for x in hw if x), "read": max(x for x in hr if x)}
                res["disk"] = {"guest_mb_s": g, "host_mb_s": h, "pct": {k: round(100 * g[k] / h[k], 1) for k in g}}
                print("disk: %s" % res["disk"], flush=True)
            if "net" in parts:
                ip3 = os.environ.get("IPERF3") or shutil.which("iperf3")
                if not ip3:
                    res["net"] = "skipped: no iperf3 on this computer (IPERF3=path)"
                else:
                    o, _ = con.run("command -v iperf3 || (apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq iperf3) >/dev/null 2>&1; "
                                   "command -v iperf3", 600)
                    if "iperf3" not in o:
                        res["net"] = "skipped: iperf3 didn't install in the guest"
                    else:
                        con.run("(iperf3 -s -D >/dev/null 2>&1 &)", 20)
                        time.sleep(2)
                        n = {}
                        for k, flag in (("to_guest", ""), ("from_guest", "-R")):
                            r = sh("%s -c 127.0.0.1 -p %d -t 10 -J %s" % (ip3, PORT, flag), 60)
                            try:
                                j = json.loads(r.stdout)
                                n[k + "_gbit_s"] = round(j["end"]["sum_received"]["bits_per_second"] / 1e9, 2)
                            except (ValueError, KeyError):
                                n[k + "_gbit_s"] = None
                        res["net"] = n
                print("net: %s" % res.get("net"), flush=True)
            if "pause" in parts:
                p, r_ = [], []
                for _ in range(10):
                    t0 = time.time(); L.vm("--pause", M, timeout=30); p.append((time.time() - t0) * 1000)
                    time.sleep(0.5)
                    t0 = time.time(); L.vm("--resume", M, timeout=30); r_.append((time.time() - t0) * 1000)
                    time.sleep(0.5)
                res["pause_ms"] = {"pause_median": round(median(p)), "pause_max": round(max(p)), "resume_median": round(median(r_)),
                                   "resume_max": round(max(r_))}
                print("pause: %s" % res["pause_ms"], flush=True)
            con.close()
            L.off(M)
            shutil.rmtree(M, ignore_errors=True)
        if "suspend" in parts:
            M = L.make("suspend8", "--accel", "kvm", "--vcpus", vcpus, "--memory", "8192", "--firmware", "uefi")
            L.boot(M)
            con = L.console(M)
            con.run("mkdir -p /mnt/f && mount -t tmpfs -o size=7g tmpfs /mnt/f && head -c 7168M /dev/urandom > /mnt/f/x; free -m | sed -n 2p", 600)
            con.close()
            t0 = time.time()
            L.vm("--suspend", M, timeout=300)
            end = time.time() + 300
            while time.time() < end and L.status(M).get("state") not in ("suspended", "stopped", "crashed"):
                time.sleep(0.2)
            s_wall = time.time() - t0
            st = L.status(M).get("state")
            t0 = time.time()
            L.vm("--start", M, "--detach", "--keep-running", timeout=300)
            end = time.time() + 300
            while time.time() < end and L.status(M).get("state") != "running":
                time.sleep(0.2)
            r_run = time.time() - t0
            con = L.console(M)
            out, _ = con.run("wc -c < /mnt/f/x", 60)
            r_con = time.time() - t0
            con.close()
            log = open(os.path.join(M, "log", "lord-vm.log"), errors="replace").read()
            ms = re.findall(r"suspend: (\d+) MB saved in (\d+) ms", log)
            ml = re.findall(r"resume: loaded in (\d+) ms", log)
            res["suspend"] = {"state_after": st, "state_file_mb": int(ms[-1][0]) if ms else None,
                              "saved_ms": int(ms[-1][1]) if ms else None, "suspend_wall_s": round(s_wall, 1),
                              "loaded_ms": int(ml[-1]) if ml else None, "resume_to_running_s": round(r_run, 1),
                              "resume_to_console_s": round(r_con, 1), "data_kept": out.strip().endswith("7516192768")}
            print("suspend: %s" % res["suspend"], flush=True)
            L.off(M)
    finally:
        L.close()
    if jout:
        json.dump(res, open(jout, "w"), indent=1)
    print(json.dumps(res))
    return 0


if __name__ == "__main__":
    sys.exit(main())
