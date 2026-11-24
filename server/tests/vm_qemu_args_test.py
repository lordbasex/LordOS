# vm_qemu_args_test.py — Machines' settings → QEMU arguments, as a table (docs/VIRTUALIZATION.md 12, "Unit"): for each row a
# throwaway machine is made with lord-vm --create and --set, started paused (-S: no guest code runs, no system on its disk),
# and the QEMU command line lord-vm logged is checked for what that setting must add and must not. Rows for this computer's
# architecture only (the arm64 rows run on the Pi). lord-vm runs in its own user and mount namespaces with a private home
# (tools/vm/e2e/machines/mh.py's ns_prefix), as the other Machines scripts do.
#   python3 -I tests/vm_qemu_args_test.py [ROW ...]      (from server/; exit 77 without KVM, QEMU, qemu-img or unshare)
import json, os, re, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(SERVER, "..", "tools", "vm", "e2e", "machines"))
from mh import ns_prefix, realhome  # noqa: E402

ARCH = {"x86_64": "amd64", "aarch64": "arm64"}.get(os.uname().machine, "")
QEMU = {"amd64": "qemu-system-x86_64", "arm64": "qemu-system-aarch64"}.get(ARCH, "")
VM = os.path.join(SERVER, "build", "lord-vm")


def skip(why):
    print("skipped: %s" % why)
    sys.exit(77)


if not ARCH:
    skip("an amd64 or arm64 computer")
if not os.access(VM, os.X_OK):
    skip("build/lord-vm")
for t in (QEMU, "qemu-img", "passt", "unshare"):
    if not shutil.which(t):
        skip(t)
if not os.access("/dev/kvm", os.R_OK | os.W_OK):
    skip("KVM")
if subprocess.run(["unshare", "-Urm", "true"], capture_output=True).returncode:
    skip("unprivileged user namespaces")

RENDER = next((p for p in ("/dev/dri/renderD128", "/dev/dri/renderD129") if os.access(p, os.R_OK | os.W_OK)), None)
GL = ARCH == "amd64" and all(os.path.exists("/usr/lib/x86_64-linux-gnu/qemu/" + m)
                             for m in ("ui-opengl.so", "hw-display-virtio-vga-gl.so", "hw-display-virtio-gpu-gl.so"))


def has(*subs):            # every piece is somewhere in the command line
    return ("has " + " & ".join(subs), lambda q, a: all(s in q for s in subs))


def lacks(*subs):
    return ("lacks " + " | ".join(subs), lambda q, a: not any(s in q for s in subs))


def opt(name, value):      # "-name value" as two arguments
    return ("%s %s" % (name, value), lambda q, a: any(a[i] == name and a[i + 1] == value for i in range(len(a) - 1)))


def opt_re(name, rx):
    r = re.compile(rx)
    return ("%s ~ %s" % (name, rx), lambda q, a: any(a[i] == name and r.search(a[i + 1]) for i in range(len(a) - 1)))


def before(x, y):          # bootindex of the device named x is lower than y's
    def f(q, a):
        bx = re.search(r"id=%s[^ ]*bootindex=(\d+)" % x, q)
        by = re.search(r"id=%s[^ ]*bootindex=(\d+)" % y, q)
        return bool(bx and by and int(bx.group(1)) < int(by.group(1)))
    return ("%s boots before %s" % (x, y), f)


# name, lord-vm --create args, lord-vm --set args, --start args, checks; a row is skipped when its "when" is false
ROWS = [
    ("defaults (UEFI, virtio, shared network, a window)", ["--display", "window"], ["--transfer", "off"], [], [
        opt("-accel", "kvm,device=/dev/fdset/1"), opt("-cpu", "host"), opt_re("-machine", r"pflash0=fw-code,pflash1=fw-vars"),
        opt_re("-machine", r"dump-guest-core=off"), has("OVMF_CODE" if ARCH == "amd64" else "AAVMF_CODE"),
        has("virtio-blk-pci,id=sysdisk,drive=sys", "iothread=disks"), has("virtio-scsi-pci,id=scsi0,addr=0x8"),
        has("-netdev stream,id=pst0,addr.type=fd,addr.str=6", "hubport,id=nic0,hubid=0"), has("virtio-net-pci,netdev=nic0"),
        has("virtio-rng-pci,addr=0x3"), opt_re("-display", r"^dbus,p2p=yes,audiodev=snd0$"),
        has("intel-hda,id=sound0,addr=0x9", "hda-micro"), opt_re("-sandbox", r"^on,obsolete=deny,elevateprivileges=deny,spawn=deny,resourcecontrol=deny$"),
        has("-nodefaults", "-no-user-config"), lacks("tpm-crb", "hv_relaxed", "vhost-user-fs-pci", "smm=on", "-boot ")]
     + ([has("virtio-vga,id=video0,addr=0x4"), has("virtio-tablet-pci,addr=0x5"), has("virtio-mouse-pci,addr=0x14")] if ARCH == "amd64" else
        [has("virtio-gpu-pci,id=video0,addr=0x4"), has("virtio-keyboard-pci,addr=0x5"), has("virtio-tablet-pci,addr=0x6")]), True),
    ("BIOS, no display, isolated network", ["--firmware", "bios", "--display", "none", "--network", "isolated"], ["--transfer", "off"], [], [
        lacks("pflash0", "fw-code-file", "-netdev stream", "virtio-net-pci"), opt("-display", "none"), opt("-audiodev", "none,id=snd0")],
     ARCH == "amd64"),
    ("host only network", ["--network", "hostonly", "--display", "none"], ["--transfer", "off"], [], [
        has("-netdev stream,id=pst0", "virtio-net-pci,netdev=nic0")], True),
    ("emulated (TCG)", ["--accel", "tcg", "--display", "none"], ["--transfer", "off"], [], [
        opt("-accel", "tcg,thread=multi,tb-size=256"), opt("-cpu", "max"), lacks("/dev/fdset/1")], True),
    ("CPU and memory", ["--vcpus", "3", "--memory", "1536", "--display", "none"], ["--transfer", "off"], [], [
        opt("-smp", "3"), opt("-m", "1536")], True),
    ("compatible devices", ["--devices", "compat", "--display", "window"], ["--transfer", "off"], [], [
        has("ide-hd,id=sysdisk,drive=sys,bus=" + ("ide.0" if ARCH == "amd64" else "ahci.0")), has("qemu-xhci,id=xhci,addr=0x5"),
        has("usb-tablet,bus=xhci.0"), has("usb-mouse,bus=xhci.0"), lacks("virtio-blk-pci,id=sysdisk")], True),
    ("Windows 11 (secure boot, TPM, Hyper-V, local clock)",
     ["--devices", "windows", "--secure-boot", "on", "--tpm", "on", "--hyperv", "on", "--rtc", "local", "--display", "window"], ["--transfer", "off"], [], [
        opt_re("-machine", r"smm=on"), has("driver=cfi.pflash01,property=secure,value=on"), opt_re("-cpu", r"^host,hv_relaxed,.*hv_ipi$"),
        opt("-rtc", "base=localtime"), has("-tpmdev emulator,id=tpm0,chardev=chrtpm", "tpm-crb,tpmdev=tpm0"),
        has("ide-cd,id=cd0,bus=ide.5"), has("qemu-xhci,id=xhci,addr=0x5"), has("virtio-blk-pci,id=sysdisk"), has("secboot")],
     ARCH == "amd64" and shutil.which("swtpm") and os.path.exists("/usr/share/OVMF/OVMF_CODE_4M.secboot.fd")),
    ("guest agent on", ["--agent", "on", "--display", "none"], ["--transfer", "off"], [], [
        has("virtio-serial-pci,id=vser0,addr=0x7"), has("name=org.qemu.guest_agent.0"), has("name=org.lordos.agent")], True),
    ("guest agent off", ["--agent", "off", "--display", "none"], ["--transfer", "off"], [], [
        lacks("org.qemu.guest_agent.0", "org.lordos.agent", "virtio-serial-pci")], True),
    ("boot order and menu", ["--display", "none"], ["--transfer", "off", "--boot-order", "cd,disk", "--boot-menu", "on"], [], [
        opt("-boot", "menu=on,splash-time=5000"), before("cd0", "sysdisk")], True),
    ("a shared folder and the transfer folder", ["--display", "none"], ["--share", "rw:docs:{share}", "--transfer", "on"], [], [
        has("vhost-user-fs-pci,id=share0,chardev=vfs0,tag=docs,addr=0xb"), has("tag=LordOS,addr=0xf"),
        opt_re("-object", r"share=on")], os.path.exists("/usr/libexec/virtiofsd")),
    ("3D graphics", ["--display", "window", "--graphics", "3d"], ["--transfer", "off"], ["--render-node", RENDER or ""], [
        has("virtio-vga-gl,id=video0,addr=0x4"), opt_re("-display", r"^dbus,p2p=yes,gl=on,rendernode=/proc/self/fd/\d+,audiodev=snd0$")],
     bool(RENDER and GL)),
]


def main():
    only = sys.argv[1:]
    D = tempfile.mkdtemp(prefix="lqargs.", dir=os.environ.get("TMPDIR", "/var/tmp"))
    os.chmod(D, 0o700)
    for d in ("home/Machines", "run", "fake/Shared"):
        os.makedirs(os.path.join(D, d))
    env = {"PATH": "/usr/bin:/bin", "HOME": os.path.join(D, "home"), "XDG_RUNTIME_DIR": os.path.join(D, "run"), "LANG": "C.UTF-8"}

    def vm(*a, timeout=120):
        return subprocess.run(ns_prefix(D) + [VM] + list(a), cwd=SERVER, env=env, capture_output=True, text=True, timeout=timeout)
    fails = passed = 0
    try:
        for i, (name, create, sets, start, checks, when) in enumerate(ROWS):
            if only and not any(o.lower() in name.lower() for o in only):
                continue
            if not when:
                print("skip: %s (not on this computer)" % name)
                continue
            M = os.path.join(D, "home", "Machines", "row%d.lordvm" % i)
            r = vm("--create", M, "--name", "row%d" % i, "--disk", "1", *create)
            if r.returncode:
                print("FAIL: %s: lord-vm --create: %s" % (name, (r.stdout + r.stderr).strip()[-400:])); fails += 1; continue
            # lord-vm shares only folders in the account's home (its passwd entry): in the namespace that is the empty folder put
            # over the real one, D/fake, so nothing of the person's is shared
            sets = [a.replace("{share}", os.path.join(realhome(), "Shared")) for a in sets]
            if sets:
                r = vm("--set", M, *sets)
                if r.returncode:
                    print("FAIL: %s: lord-vm --set: %s" % (name, (r.stdout + r.stderr).strip()[-400:])); fails += 1; continue
            vm("--start", M, "--paused", "--detach", *start)
            logf = os.path.join(M, "log", "lord-vm.log")
            line = None
            end = time.time() + 40
            while time.time() < end:
                t = open(logf, errors="replace").read() if os.path.exists(logf) else ""
                if "qemu: pid" in t or "state crashed" in t:
                    break
                time.sleep(0.3)
            # the command line, logged in pieces ("qemu: <binary> …" then "qemu+ …" lines)
            q = None
            for l in t.splitlines():
                m0 = re.search(r"qemu: (\S*%s .*)" % QEMU, l)
                if m0:
                    q = m0.group(1); continue
                m1 = re.search(r"qemu\+ (.*)", l)
                if m1 and q is not None:
                    q += m1.group(1)
            vm("--poweroff", M, timeout=60)
            if not q:
                print("FAIL: %s: no QEMU command line in the log: %s" % (name, t.strip().splitlines()[-3:] if t else "no log")); fails += 1; continue
            args = q.split(" ")
            bad = [d for d, f in checks if not f(q, args)]
            if bad:
                fails += 1
                print("FAIL: %s: %s" % (name, "; ".join(bad)))
                print("      %s" % q[:3000])
            else:
                passed += 1
                print("ok: %s (%d checks)" % (name, len(checks)))
            shutil.rmtree(M, ignore_errors=True)
    finally:
        for m in os.listdir(os.path.join(D, "home", "Machines")):
            vm("--poweroff", os.path.join(D, "home", "Machines", m), timeout=60)
        shutil.rmtree(D, ignore_errors=True)
    print("== vm-qemu-args: %d passed, %d failed" % (passed, fails))
    return 1 if fails else 0


sys.exit(main())
