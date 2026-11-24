# kernel-boot.py — direct kernel boot of an installed Linux guest (docs/VIRTUALIZATION.md 6.1, "Start Linux directly";
# machine.json schema 21), end to end with real QEMU, in a scratch library in private user and mount namespaces (mh.py):
#
#   1. the harness's Debian 13 guest (images.py) imported with UEFI (and again with BIOS on amd64), its agent and
#      automation on; started cold from its disk: seconds to "MH-READY" on its serial console (firmware, GRUB's menu and
#      countdown, the kernel);
#   2. the guest's own kernel, initrd and command line taken out of it the documented way: `machines cp NAME:/vmlinuz`,
#      `NAME:/initrd.img` and `machines exec NAME -- cat /proc/cmdline` (as the signed-in person, through the agent);
#   3. shut down, `lord-vm --set --kernel --initrd --cmdline --kernel-boot on`; started again cold: seconds to MH-READY,
#      lord-vm's log saying it ran disks/boot's kernel, the agent connecting, the guest's /proc/cmdline the one chosen;
#   4. what is refused: a bad command line, a file that isn't a kernel, kernel_boot with no kernel yet, a Secure Boot
#      machine, the switch while it runs;
#   5. a clone keeps disks/boot and boots directly; --kernel-boot off starts from the disk again.
#
#   python3 -I tools/vm/e2e/machines/kernel-boot.py [--json FILE] [--firmware uefi,bios]
# Exit 0 when every check passed, 1 otherwise, 77 when it can't run here (no KVM, no image, no build).
import json, os, re, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mh  # noqa: E402
import images  # noqa: E402

VM = os.path.join(mh.BUILD, "lord-vm")
CLI = os.path.join(mh.BUILD, "machines")
ARCH = images.ARCH


def main():
    a = sys.argv[1:]
    jout = a[a.index("--json") + 1] if "--json" in a else None
    fws = (a[a.index("--firmware") + 1] if "--firmware" in a else ("uefi,bios" if ARCH == "amd64" else "uefi")).split(",")
    if not (os.access(VM, os.X_OK) and os.access(CLI, os.X_OK)):
        print("skipped: needs server/build/lord-vm and server/build/machines"); return 77
    if not mh.kvm_ok():
        print("skipped: needs /dev/kvm"); return 77
    if subprocess.run(["unshare", "-Urm", "true"], capture_output=True).returncode:
        print("skipped: needs unprivileged user namespaces"); return 77
    try:
        img = images.agent_image(ARCH)
    except mh.Skip as e:
        print("skipped: %s" % e); return 77
    os.makedirs(mh.SCRATCH, exist_ok=True)
    D = tempfile.mkdtemp(prefix="kboot.", dir=mh.SCRATCH)
    os.chmod(D, 0o700)
    for d in ("home/Machines", "run", "fake"):
        os.makedirs(os.path.join(D, d))
    H = os.path.join(D, "home")
    env = {"PATH": "/usr/bin:/bin", "HOME": H, "XDG_RUNTIME_DIR": os.path.join(D, "run"), "LANG": "C.UTF-8", "LORD_VM": VM}
    res = {"arch": ARCH, "rows": []}
    fails = []

    def run(prog, *args, timeout=600):
        return subprocess.run(mh.ns_prefix(D) + [prog] + list(args), cwd=mh.SERVER, env=env, capture_output=True, text=True, timeout=timeout, stdin=subprocess.DEVNULL)

    def vm(*args, timeout=600):
        return run(VM, *args, timeout=timeout)

    def check(what, ok, detail=""):
        print("%s  %s%s" % ("PASS" if ok else "FAIL", what, (": " + detail) if detail and not ok else ""), flush=True)
        if not ok:
            fails.append(what)
        return ok

    def status(M):
        try:
            return json.loads(vm("--status", M, "--json", timeout=30).stdout or "{}")
        except ValueError:
            return {}

    def serial_count(M):
        p = os.path.join(M, "log", "serial.log")
        return open(p, errors="replace").read().count("MH-READY") if os.path.exists(p) else 0

    def boot(M, *start):
        """a cold start to MH-READY: its seconds, or None"""
        n0 = serial_count(M)
        t0 = time.time()
        r = vm("--start", M, "--detach", "--keep-running", *start)
        if r.returncode:
            print("      start: " + (r.stdout + r.stderr).strip()[-300:])
            return None
        while time.time() - t0 < 240:
            if serial_count(M) > n0:
                return round(time.time() - t0, 2)
            if status(M).get("state") in ("crashed", "stopped") and time.time() - t0 > 5:
                return None
            time.sleep(0.2)
        return None

    def agent_up(M, tmo=90):
        end = time.time() + tmo
        while time.time() < end:
            try:
                if json.load(open(os.path.join(M, "run.json"))).get("agent") == "connected":
                    return True
            except (OSError, ValueError):
                pass
            time.sleep(0.5)
        return False

    def qemu_args(M):
        """the running QEMU's arguments (no pid namespace: its /proc entry is this computer's)"""
        try:
            return open("/proc/%d/cmdline" % int(status(M).get("qemu_pid") or 0), "rb").read().split(b"\0")
        except (OSError, ValueError):
            return []

    def stop(M):
        vm("--stop", M, "--timeout", "90", timeout=150)
        end = time.time() + 120
        while time.time() < end and status(M).get("state") not in ("stopped", "crashed"):
            time.sleep(0.5)
        vm("--poweroff", M, timeout=60)

    try:
        for fw in fws:
            name = "kboot-" + fw
            M = os.path.join(H, "Machines", name + ".lordvm")
            rec = {"firmware": fw}
            res["rows"].append(rec)
            r = vm("--create", M, "--name", name, "--arch", ARCH, "--accel", "kvm", "--firmware", fw, "--vcpus", "2", "--memory", "2048",
                   "--agent", "on", "--display", "window", "--import", img)
            if not check("%s: the machine is made" % fw, r.returncode == 0, (r.stdout + r.stderr)[-300:]):
                continue
            r = vm("--set", M, "--automation", "on")
            check("%s: automation on" % fw, r.returncode == 0, r.stderr[-200:])
            # 4. refusals before any file is there
            r = vm("--set", M, "--kernel-boot", "on")
            check("%s: kernel_boot with no kernel yet is refused" % fw, r.returncode != 0 and "choose the kernel" in r.stderr, r.stderr)
            notk = os.path.join(H, "notakernel")
            open(notk, "wb").write(b"\0" * 4096)
            r = vm("--set", M, "--kernel", notk)
            check("%s: a file that isn't a kernel is refused" % fw, r.returncode != 0 and "isn't a Linux kernel" in r.stderr, r.stderr)
            r = vm("--set", M, "--cmdline", "root=/dev/vda1\x01")
            check("%s: a command line with a control character is refused" % fw, r.returncode != 0, r.stderr)
            # 1. from the disk
            t_disk = boot(M)
            rec["disk_boot_s"] = t_disk
            if not check("%s: boots from its disk to MH-READY" % fw, t_disk is not None, str(status(M))):
                continue
            check("%s: the agent connects" % fw, agent_up(M))
            # the switch while it runs: refused (the machine's lock)
            r = vm("--set", M, "--kernel-boot", "on")
            check("%s: the switch is refused while it runs" % fw, r.returncode != 0, r.stderr)
            # 2. the guest's own kernel, initrd and command line, taken out of it
            k, i = os.path.join(H, "vmlinuz-" + fw), os.path.join(H, "initrd-" + fw)
            # the running kernel's own files (a cloud image has no /vmlinuz link: /boot/vmlinuz-<uname -r>)
            rel = run(CLI, "exec", name, "--", "uname", "-r", timeout=60).stdout.strip()
            r1 = run(CLI, "cp", "%s:/boot/vmlinuz-%s" % (name, rel), k, timeout=300)
            r2 = run(CLI, "cp", "%s:/boot/initrd.img-%s" % (name, rel), i, timeout=300)
            r3 = run(CLI, "exec", name, "--", "cat", "/proc/cmdline", timeout=60)
            ok = check("%s: machines cp takes the kernel and initrd out" % fw, r1.returncode == 0 and r2.returncode == 0 and os.path.exists(k) and os.path.getsize(k) > 1 << 20,
                       (r1.stdout + r1.stderr + r2.stderr)[-300:])
            cmd = re.sub(r"BOOT_IMAGE=\S+\s*", "", r3.stdout.strip())
            rec["guest_cmdline"] = cmd
            rec["guest_kernel"] = rel
            check("%s: machines exec reads /proc/cmdline" % fw, r3.returncode == 0 and "root=" in cmd, r3.stderr[-200:])
            stop(M)
            if not ok:
                continue
            # all or nothing: a --set whose initrd is missing leaves disks/boot as it was
            r = vm("--set", M, "--kernel", k, "--initrd", os.path.join(H, "missing"), "--cmdline", "root=/dev/sda1")
            check("%s: a refused --set changes nothing in disks/boot" % fw, r.returncode != 0 and not os.listdir(os.path.join(M, "disks", "boot")),
                  str(os.listdir(os.path.join(M, "disks", "boot"))))
            # 3. the switch, then a cold start from the kernel
            mark = cmd + " lordos.kboot=1"
            r = vm("--set", M, "--kernel", k, "--initrd", i, "--cmdline", mark + "\n", "--kernel-boot", "on")
            check("%s: --set takes the kernel, the initrd and the command line" % fw, r.returncode == 0, r.stderr)
            mj = json.load(open(os.path.join(M, "machine.json")))
            check("%s: machine.json has kernel_boot and schema 21" % fw, mj.get("kernel_boot") is True and mj.get("schema", 0) >= 21, str(mj.get("schema")))
            check("%s: disks/boot holds the copies (0600, not links)" % fw,
                  all(os.path.isfile(os.path.join(M, "disks/boot", f)) and not os.path.islink(os.path.join(M, "disks/boot", f))
                      and (os.stat(os.path.join(M, "disks/boot", f)).st_mode & 0o077) == 0 for f in ("vmlinuz", "initrd", "cmdline")))
            check("%s: the command line is kept without its newline" % fw, open(os.path.join(M, "disks/boot/cmdline")).read() == mark)
            t_k = boot(M)
            rec["kernel_boot_s"] = t_k
            log = open(os.path.join(M, "log", "lord-vm.log"), errors="replace").read()
            check("%s: boots the kernel directly to MH-READY" % fw, t_k is not None, str(status(M)))
            qa = qemu_args(M)
            check("%s: lord-vm's log says it ran disks/boot's kernel" % fw, "kernel boot: this start runs disks/boot's kernel and initrd" in log)
            check("%s: QEMU runs with -kernel, -initrd and -append from disks/boot" % fw, b"-kernel" in qa and any(a.endswith(b"disks/boot/vmlinuz") for a in qa) and b"-initrd" in qa, str(qa[:4]))
            check("%s: the agent connects" % fw, agent_up(M))
            r = run(CLI, "exec", name, "--", "cat", "/proc/cmdline", timeout=60)
            check("%s: the guest runs with the chosen command line" % fw, "lordos.kboot=1" in r.stdout, r.stdout + r.stderr)
            stop(M)
            # 5. a clone keeps disks/boot; off goes back to the disk
            C = os.path.join(H, "Machines", name + "-clone.lordvm")
            r = vm("--clone", M, C, "--name", name + "-clone", timeout=600)
            if check("%s: a full clone is made" % fw, r.returncode == 0, (r.stdout + r.stderr)[-300:]):
                check("%s: the clone has disks/boot" % fw, all(os.path.isfile(os.path.join(C, "disks/boot", f)) for f in ("vmlinuz", "initrd", "cmdline")))
                t_c = boot(C)
                clog = open(os.path.join(C, "log", "lord-vm.log"), errors="replace").read()
                check("%s: the clone boots its kernel directly" % fw, t_c is not None and b"-kernel" in qemu_args(C))
                stop(C)
                shutil.rmtree(C, ignore_errors=True)
            r = vm("--set", M, "--kernel-boot", "off")
            check("%s: --kernel-boot off" % fw, r.returncode == 0, r.stderr)
            t_off = boot(M)
            log = open(os.path.join(M, "log", "lord-vm.log"), errors="replace").read()
            check("%s: off: it starts from its disk again" % fw, t_off is not None and b"-kernel" not in qemu_args(M) and len(qemu_args(M)) > 0)
            stop(M)
            rec["saved_s"] = round(t_disk - t_k, 2) if t_disk and t_k else None
            print("      %s: from the disk %s s, the kernel directly %s s" % (fw, t_disk, t_k), flush=True)
        # Secure Boot refuses it (amd64: OVMF's MS keys)
        if ARCH == "amd64" and shutil.which("swtpm") and os.path.exists("/usr/share/OVMF/OVMF_CODE_4M.secboot.fd"):
            S = os.path.join(H, "Machines", "kboot-sb.lordvm")
            r = vm("--create", S, "--name", "kboot-sb", "--arch", ARCH, "--accel", "kvm", "--secure-boot", "on", "--tpm", "on", "--vcpus", "1", "--memory", "1024", "--disk", "1")
            if check("sb: a Secure Boot machine is made", r.returncode == 0, r.stderr[-200:]):
                r = vm("--set", S, "--cmdline", "root=/dev/vda1")
                check("sb: kernel boot is refused with Secure Boot", r.returncode != 0 and "secure boot" in r.stderr, r.stderr)
    finally:
        for p in os.listdir(os.path.join(H, "Machines")):
            vm("--poweroff", os.path.join(H, "Machines", p), timeout=60)
        if os.environ.get("MH_KEEP") != "1":
            shutil.rmtree(D, ignore_errors=True)
        else:
            print("kept: " + D)
    res["failed"] = fails
    if jout:
        json.dump(res, open(jout, "w"), indent=1)
    print("kernel-boot: %d failed" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
