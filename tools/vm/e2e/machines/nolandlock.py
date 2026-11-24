# nolandlock.py — the "Weaker isolation on this kernel" notice (docs/VIRTUALIZATION.md 3.3) seen on a kernel without
# Landlock. Every computer here has Landlock, so the harness's Debian 13 guest (images.py) is the computer: started by
# this build's lord-vm in a scratch library (private namespaces, like boot-matrix.py), its kernel told to start without
# Landlock (`lsm=` without it, through GRUB), this source tree shared into it read only (virtiofs), and the Machines
# harness (mh.py, a headless lord-server-test) run inside it as its person (tester). Inside: the library's notice,
# Configure → Security's sandbox line, and a machine started there (emulated: TCG) whose `lord-vm --status` says what
# its sandbox has. Pictures are copied to OUT. Test only: nothing here ships.
#
#   python3 -I tools/vm/e2e/machines/nolandlock.py [--out DIR] [--tree COPY]   (COPY: the tree elsewhere, when its path has a
#   hidden folder, which Machines won't share)
# The same guest runs other checks that need a computer of their own (a package changed under a machine):
#   nolandlock.py --inner pkgchange_inner.py --keep-landlock --as root     (pkgchange_inner.py says what it checks)
#   nolandlock.py --inner bridged_inner.py --keep-landlock --as root --image   (--image: the guest image in the tree's copy)
# Needs KVM, the guest image (images.py) and a built tree (make -C server, npm install). About 10 minutes with the network.
import os, re, shutil, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mh  # noqa: E402
from targets import Lib  # noqa: E402

PKGS = ("nodejs python3 qemu-system-x86 qemu-system-modules-opengl qemu-utils ovmf passt libegl1 libgles2 libgbm1 libwayland-client0 libwayland-server0 "
        "libxkbcommon0 libcap2 libexpat1")

def main():
    a = sys.argv[1:]
    out = os.path.abspath(a[a.index("--out") + 1] if "--out" in a else os.path.join(mh.SCRATCH, "nolandlock-shots"))
    tree = os.path.abspath(a[a.index("--tree") + 1]) if "--tree" in a else mh.ROOT    # a copy when ROOT's path has a hidden folder
    inner = a[a.index("--inner") + 1] if "--inner" in a else "nolandlock_inner.py"
    who = a[a.index("--as") + 1] if "--as" in a else "tester"
    keep = "--keep-landlock" in a
    if not mh.kvm_ok():
        print("skipped: no /dev/kvm"); return 77
    L = Lib()
    rc = 1
    try:
        M = L.make("nolandlock", "--accel", "kvm", "--vcpus", "4", "--memory", "6144")
        # shares are folders in the person's home (lord-vm reads it from the account; in the namespaces it is the empty
        # folder mounted over it, L.D/fake): the tree (hard links when it can) and a folder for the pictures
        src, pics = os.path.join(L.D, "fake", "src"), os.path.join(L.D, "fake", "out")
        if subprocess.run(["cp", "-al", tree, src], capture_output=True).returncode:
            shutil.copytree(tree, src, symlinks=True, ignore=shutil.ignore_patterns(".git"))
        if "--image" in a:                    # the harness's guest image too, for an inner script that runs a machine
            import images
            try:
                os.link(images.agent_image(images.ARCH), os.path.join(src, ".mh-image.qcow2"))
            except OSError:
                shutil.copy(images.agent_image(images.ARCH), os.path.join(src, ".mh-image.qcow2"))
        os.makedirs(pics)
        os.chmod(pics, 0o777)
        r = L.vm("--set", M, "--share", "ro:src:%s/src" % mh.realhome(), "--share", "rw:out:%s/out" % mh.realhome())
        if r.returncode:
            raise RuntimeError(r.stdout + r.stderr)
        L.boot(M)
        con = L.console(M)
        o, st = con.run("DEBIAN_FRONTEND=noninteractive apt-get update -qq && DEBIAN_FRONTEND=noninteractive "
                        "apt-get install -y -qq --no-install-recommends %s >/tmp/apt.log 2>&1; echo APT=$?" % PKGS, 1800)
        print(o[-200:], flush=True)
        if not keep:
            o, _ = con.run("cat /sys/kernel/security/lsm; echo; sed -i 's/^GRUB_CMDLINE_LINUX=\"/GRUB_CMDLINE_LINUX=\"lsm=lockdown,capability,yama,apparmor /' "
                           "/etc/default/grub && update-grub 2>&1 | tail -1", 300)
            print(o, flush=True)
            con.close()
            L.vm("--stop", M, "--timeout", "60", timeout=120)
            end = time.time() + 90
            while time.time() < end and L.status(M).get("state") not in ("stopped", "crashed"):
                time.sleep(0.5)
            L.boot(M)
            con = L.console(M)
        o, _ = con.run("cat /sys/kernel/security/lsm; echo; cat /proc/cmdline", 30)
        print("guest:", o, flush=True)
        con.run("mkdir -p /mnt/src && mount -t virtiofs src /mnt/src && chmod 666 /dev/kvm 2>/dev/null; true", 30)
        con.run("mkdir -p /mnt/out && mount -t virtiofs out /mnt/out && chmod 777 /mnt/out", 30)
        cmd = "MH_SCRATCH=/var/tmp/nll MH_OUT=/mnt/out python3 -I /mnt/src/tools/vm/e2e/machines/%s" % inner
        o, _ = con.run(("%s 2>&1 | tail -60" % cmd) if who == "root" else ("su - tester -c '%s' 2>&1 | tail -60" % cmd), 3600)
        print(o, flush=True)
        rc = 0 if re.search(r"\b0 failed", o) else 1
        os.makedirs(out, exist_ok=True)
        for f in os.listdir(pics):
            shutil.copy(os.path.join(pics, f), out)
        print("pictures in", out)
        con.close()
    finally:
        L.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
