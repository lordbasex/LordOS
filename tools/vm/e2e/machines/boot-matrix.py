# boot-matrix.py — Machines' boot matrix (docs/VIRTUALIZATION.md 12): the harness's Debian 13 guest (images.py) booted by
# lord-vm under each engine setting this computer can run, each in a scratch library in private user and mount
# namespaces (mh.py). For each row: the machine made (--import of the cached image), started, the seconds to the guest's
# "MH-READY" on its serial console, whether qemu-guest-agent connected, a clean Shut Down (the agent, then ACPI) and its
# seconds, the disk's size; then the machine is removed.
#
#   python3 -I tools/vm/e2e/machines/boot-matrix.py [--rows a,b,…] [--json FILE] [--md FILE]
#
# Rows (each skipped, with why, when this computer can't run it):
#   kvm-uefi-2d        KVM, OVMF/AAVMF, 2D (virtio-vga / virtio-gpu)        tcg-uefi-2d    the same emulated (TCG)
#   kvm-bios-2d        KVM, SeaBIOS (amd64)                                  tcg-bios-2d    the same emulated
#   kvm-uefi-tpm       KVM, UEFI, a TPM 2.0 (swtpm)                          kvm-uefi-sb    KVM, Secure Boot (MS keys) and a TPM
#   kvm-uefi-3d-<card> KVM, UEFI, 3D (virgl) on each render node             kvm-compat     KVM, the compatible devices
#   other-tcg          the other architecture, emulated (arm64 on amd64, amd64 on arm64), with its own image
#   tcg-compat         the compatible devices emulated      other-tcg-compat-<arch>  the same, the other architecture
#                      (amd64: SATA; arm64: NVMe and a USB CD/DVD, AAVMF has no SATA driver)
# Exit 0 when every row that ran booted; 1 otherwise; 77 when nothing could run.
import json, os, re, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mh  # noqa: E402
import images  # noqa: E402

ARCH = images.ARCH
OTHER = {"amd64": "arm64", "arm64": "amd64"}[ARCH]
QEMU = {"amd64": "qemu-system-x86_64", "arm64": "qemu-system-aarch64"}
VM = os.path.join(mh.BUILD, "lord-vm")


def render_nodes():
    out = []
    for n in sorted(os.listdir("/dev/dri")) if os.path.isdir("/dev/dri") else []:
        if n.startswith("renderD") and os.access("/dev/dri/" + n, os.R_OK | os.W_OK):
            drv = ""
            try:
                drv = os.path.basename(os.readlink("/sys/class/drm/%s/device/driver" % n))
            except OSError:
                pass
            out.append(("/dev/dri/" + n, drv))
    return out


def rows():
    kvm = mh.kvm_ok()
    r = []
    add = lambda name, arch, create, start=(), why=None, tmo=None: r.append(
        {"name": name, "arch": arch, "create": list(create), "start": list(start), "why": why, "timeout": tmo})
    add("kvm-uefi-2d", ARCH, ["--accel", "kvm", "--firmware", "uefi"], why=None if kvm else "no /dev/kvm")
    add("kvm-bios-2d", ARCH, ["--accel", "kvm", "--firmware", "bios"],
        why=None if kvm and ARCH == "amd64" else ("arm64 has no BIOS" if ARCH != "amd64" else "no /dev/kvm"))
    add("kvm-uefi-tpm", ARCH, ["--accel", "kvm", "--firmware", "uefi", "--tpm", "on"],
        why=None if kvm and shutil.which("swtpm") else "needs KVM and swtpm")
    add("kvm-uefi-sb", ARCH, ["--accel", "kvm", "--secure-boot", "on", "--tpm", "on"],
        why=None if kvm and ARCH == "amd64" and shutil.which("swtpm") and os.path.exists("/usr/share/OVMF/OVMF_CODE_4M.secboot.fd")
        else "Secure Boot is amd64's (OVMF's MS keys), needs swtpm")
    add("kvm-compat", ARCH, ["--accel", "kvm", "--devices", "compat"], why=None if kvm else "no /dev/kvm")
    for node, drv in render_nodes():
        add("kvm-uefi-3d-%s" % (drv or os.path.basename(node)), ARCH, ["--accel", "kvm", "--graphics", "3d"], ["--render-node", node],
            why=None if kvm and ARCH == "amd64" else "3D is for amd64 Linux guests here")
    add("tcg-uefi-2d", ARCH, ["--accel", "tcg", "--firmware", "uefi"], tmo=900)
    add("tcg-bios-2d", ARCH, ["--accel", "tcg", "--firmware", "bios"], why=None if ARCH == "amd64" else "arm64 has no BIOS", tmo=900)
    add("other-tcg-%s" % OTHER, OTHER, ["--accel", "tcg", "--arch", OTHER],
        why=None if shutil.which(QEMU[OTHER]) else "%s isn't installed" % QEMU[OTHER], tmo=1500)
    # the compatible devices emulated: SATA on amd64; NVMe and a USB CD/DVD on arm64, where AAVMF has no SATA driver
    add("tcg-compat", ARCH, ["--accel", "tcg", "--devices", "compat"], tmo=900)
    add("other-tcg-compat-%s" % OTHER, OTHER, ["--accel", "tcg", "--arch", OTHER, "--devices", "compat"],
        why=None if shutil.which(QEMU[OTHER]) else "%s isn't installed" % QEMU[OTHER], tmo=1500)
    return r


def main():
    a = sys.argv[1:]
    only = a[a.index("--rows") + 1].split(",") if "--rows" in a else None
    jout = a[a.index("--json") + 1] if "--json" in a else None
    mdout = a[a.index("--md") + 1] if "--md" in a else None
    if not os.access(VM, os.X_OK):
        print("skipped: needs server/build/lord-vm"); return 77
    if subprocess.run(["unshare", "-Urm", "true"], capture_output=True).returncode:
        print("skipped: needs unprivileged user namespaces"); return 77
    D = tempfile.mkdtemp(prefix="matrix.", dir=mh.scratch())
    os.chmod(D, 0o700)
    for d in ("home/Machines", "run", "fake"):
        os.makedirs(os.path.join(D, d))
    env = {"PATH": "/usr/bin:/bin", "HOME": os.path.join(D, "home"), "XDG_RUNTIME_DIR": os.path.join(D, "run"), "LANG": "C.UTF-8"}

    def vm(*args, timeout=600):
        return subprocess.run(mh.ns_prefix(D) + [VM] + list(args), cwd=mh.SERVER, env=env, capture_output=True, text=True, timeout=timeout)

    def status(M):
        try:
            return json.loads(vm("--status", M, "--json", timeout=30).stdout or "{}")
        except ValueError:
            return {}
    host = {"arch": ARCH, "kernel": os.uname().release, "cpu": "", "qemu": "", "lord_vm_sha256": images.sha256(VM)[:16]}
    try:
        host["cpu"] = re.search(r"model name\s*:\s*(.*)", open("/proc/cpuinfo").read()).group(1)
    except (OSError, AttributeError):
        host["cpu"] = os.uname().machine
    q = shutil.which(QEMU[ARCH])
    host["qemu"] = subprocess.run([q, "--version"], capture_output=True, text=True).stdout.split("\n")[0] if q else ""
    res = []
    ran = failed = 0
    try:
        for row in rows():
            if only and row["name"] not in only and not any(row["name"].startswith(o) for o in only):
                continue
            rec = {"row": row["name"], "arch": row["arch"]}
            res.append(rec)
            if row["why"]:
                rec["result"] = "skipped: " + row["why"]; print("SKIP  %-22s %s" % (row["name"], row["why"])); continue
            fs = os.statvfs(D)
            if fs.f_bavail * fs.f_frsize < int(2.2 * (1 << 30)):        # the imported copy (~1.2 GB) and room to spare
                rec["result"] = "skipped: less than 2.2 GB free in %s" % D; print("SKIP  %-22s %s" % (row["name"], rec["result"][9:])); continue
            try:
                img = images.agent_image(row["arch"])
            except mh.Skip as e:
                rec["result"] = "skipped: %s" % e; print("SKIP  %-22s %s" % (row["name"], e)); continue
            M = os.path.join(D, "home", "Machines", row["name"] + ".lordvm")
            t0 = time.time()
            r = vm("--create", M, "--name", row["name"], "--arch", row["arch"], "--vcpus", "2", "--memory", "2048", "--agent", "on",
                   "--display", "window", "--import", img, *row["create"])
            rec["create_s"] = round(time.time() - t0, 1)
            if r.returncode:
                rec["result"] = "FAIL create: " + (r.stdout + r.stderr).strip()[-300:]; failed += 1
                print("FAIL  %-22s %s" % (row["name"], rec["result"])); continue
            ran += 1
            t0 = time.time()
            r = vm("--start", M, "--detach", "--keep-running", *row["start"])
            tmo = row["timeout"] or 180
            serial = os.path.join(M, "log", "serial.log")
            ready = None
            while time.time() - t0 < tmo:
                t = open(serial, errors="replace").read() if os.path.exists(serial) else ""
                if "MH-READY" in t:
                    ready = time.time() - t0; break
                st = status(M)
                if st.get("state") in ("crashed", "stopped") and time.time() - t0 > 5:
                    break
                time.sleep(1)
            st = status(M)
            log = open(os.path.join(M, "log", "lord-vm.log"), errors="replace").read() if os.path.exists(os.path.join(M, "log", "lord-vm.log")) else ""
            rec["boot_s"] = round(ready, 1) if ready else None
            gl = re.search(r"3D: (.*)", log)
            if "--graphics" in row["create"]:
                rec["gl"] = "virtio-vga-gl" in log
                rec["gl_note"] = gl.group(1)[:120] if gl else ""
            ag = None
            if ready:
                end = time.time() + 60
                while time.time() < end:
                    try:
                        rj = json.load(open(os.path.join(M, "run.json")))
                    except (OSError, ValueError):
                        rj = {}
                    if rj.get("agent") == "connected":
                        ag = round(time.time() - t0, 1); break
                    time.sleep(1)
            rec["agent_s"] = ag
            t1 = time.time()
            vm("--stop", M, "--timeout", "90", timeout=150)
            end = time.time() + 120
            while time.time() < end and status(M).get("state") not in ("stopped", "crashed"):
                time.sleep(1)
            rec["shutdown_s"] = round(time.time() - t1, 1)
            last = [l for l in open(os.path.join(M, "log", "lord-vm.log"), errors="replace").read().splitlines() if "state stopped" in l]
            rec["shutdown"] = last[-1].split("state stopped", 1)[1].strip().strip("()") if last else "?"
            vm("--poweroff", M, timeout=60)
            du = subprocess.run(["du", "-sk", os.path.join(M, "disks")], capture_output=True, text=True).stdout.split()
            rec["disk_mb"] = round(int(du[0]) / 1024) if du else None
            ok = bool(ready) and ag is not None and "forced" not in rec["shutdown"] and ("gl" not in rec or rec["gl"])
            rec["result"] = "ok" if ok else "FAIL" + ("" if ready else " (no MH-READY: %s %s)" % (st.get("state"), st.get("reason")))
            if not ok:          # what the guest said last, kept with the result (the machine is removed)
                t = open(serial, errors="replace").read() if os.path.exists(serial) else ""
                rec["serial_tail"] = [re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", l)[:160] for l in t.splitlines()[-15:]]
                for l in rec["serial_tail"]:
                    print("      | " + l)
            failed += 0 if ok else 1
            print("%s  %-22s boot %s s, agent %s s, shut down %s s (%s), disk %s MB%s" % (
                "PASS" if ok else "FAIL", row["name"], rec["boot_s"], ag, rec["shutdown_s"], rec["shutdown"], rec["disk_mb"],
                "" if "gl" not in rec else ", 3D %s" % ("on" if rec["gl"] else "OFF: " + rec.get("gl_note", ""))), flush=True)
            shutil.rmtree(M, ignore_errors=True)
    finally:
        for m in os.listdir(os.path.join(D, "home", "Machines")):
            vm("--poweroff", os.path.join(D, "home", "Machines", m), timeout=60)
        if os.environ.get("MH_KEEP") != "1":
            shutil.rmtree(D, ignore_errors=True)
    out = {"host": host, "date": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "rows": res}
    if jout:
        json.dump(out, open(jout, "w"), indent=1)
    if mdout:
        with open(mdout, "w") as f:
            f.write("| Row | Boot to MH-READY | Agent | Shut down | Disk | Result |\n|---|---:|---:|---|---:|---|\n")
            for x in res:
                f.write("| %s | %s | %s | %s | %s | %s |\n" % (x["row"], "%s s" % x["boot_s"] if x.get("boot_s") else "—",
                        "%s s" % x["agent_s"] if x.get("agent_s") else "—",
                        "%s s (%s)" % (x["shutdown_s"], x["shutdown"]) if "shutdown_s" in x else "—",
                        "%s MB" % x["disk_mb"] if x.get("disk_mb") else "—", x.get("result", "")))
    print("== boot-matrix (%s, %s): %d ran, %d failed" % (host["cpu"], host["qemu"], ran, failed))
    return 1 if failed else (0 if ran else 77)


if __name__ == "__main__":
    sys.exit(main())
