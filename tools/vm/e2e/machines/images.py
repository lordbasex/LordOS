# images.py — the guests the Machines harness boots, cached in /var/tmp/lordos-m100-images (MH_IMAGES), each made once:
#
#   debian13-<arch>.qcow2        Debian 13's "nocloud" cloud image as Debian publishes it, its SHA-512 checked against
#                                Debian's SHA512SUMS (fetched over HTTPS from cloud.debian.org) before it is kept
#   debian13-agent<N>-<arch>.qcow2  (N: PREP_VERSION) that image booted once under lord-vm (scratch namespaces, KVM) with a first-boot script
#                                given as systemd credentials: root's password set (systemd-firstboot asks otherwise),
#                                qemu-guest-agent, Xvfb and xclip installed from Debian (a display at :0 for the
#                                clipboard's test), a person's account (tester, uid 1000: dropped
#                                files go to a person's Downloads), root logged in by itself on the serial console
#                                (the harness types commands there) and "MH-READY" written to the console at each boot;
#                                then powered off and flattened. Read-only; <file>.sha256 next to it.
#
#   win11-eval-<lang>.iso        Microsoft's 90-day Windows 11 Enterprise Evaluation, downloaded from Microsoft's own server
#                                (the URL and SHA-256 the catalog pins, assets/machines/systems.json, as the assistant's
#                                evaluation card fetches it); kept only when its SHA-256 is the pinned one
#   virtio-win-<release>.iso     the virtio-win drivers' disc the catalog pins, from the project's own folder, checked the same way
#
#   python3 -I images.py [--arch amd64|arm64] [--agent]      prints the path (makes it when missing)
#   python3 -I images.py --windows [--lang en-US]           the evaluation ISO and virtio-win (about 8.9 GB)
# Exit 77 when it can't be made here (no network, no KVM): the flows that need it skip.
import hashlib, json, os, shutil, subprocess, sys, tempfile, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mh import BUILD, IMAGES, ns_prefix, private_dir, kvm_ok, Skip, skip, log  # noqa: E402

BASE = "https://cloud.debian.org/images/cloud/trixie/latest"
ARCH = {"x86_64": "amd64", "aarch64": "arm64"}.get(os.uname().machine, os.uname().machine)

PREP_VERSION = 3          # raised whenever PREP changes: a new name, so a cached older image isn't used

# the first boot's script: run once, as root, by a unit that the credentials add (lordvm-prep.service)
PREP = r"""#!/bin/sh
set -x
echo "<3>MHPREP-START" >/dev/kmsg
ok=0
for i in 1 2 3 4 5; do
  if apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends qemu-guest-agent xvfb xclip; then ok=1; break; fi
  sleep 5
done
# a person's account (uid 1000): Machines copies dropped files into a person's Downloads, never root's
id tester >/dev/null 2>&1 || useradd -m -s /bin/bash -u 1000 tester
. "$CREDENTIALS_DIRECTORY/mhprep2.sh"
"""
# its second half (a credential line holds at most 2 KB: lord-vm's --credentials-file)
PREP2 = r"""mkdir -p /etc/systemd/system/serial-getty@ttyS0.service.d /etc/systemd/system/serial-getty@ttyAMA0.service.d
for t in ttyS0 ttyAMA0; do
cat > /etc/systemd/system/serial-getty@$t.service.d/autologin.conf <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --noclear --keep-baud 115200,57600,38400,9600 %I linux
EOF
done
# a display for the clipboard's test (Xvfb :0, no TCP), where lordos-guest and xclip meet; nothing draws on it
cat > /etc/systemd/system/mh-xvfb.service <<'EOF'
[Unit]
Description=Machines harness: a display for the clipboard
[Service]
ExecStart=/usr/bin/Xvfb :0 -screen 0 1024x768x24 -nolisten tcp
[Install]
WantedBy=multi-user.target
EOF
systemctl enable mh-xvfb.service
cat > /etc/systemd/system/mh-ready.service <<'EOF'
[Unit]
Description=Machines harness: the guest is up
After=multi-user.target qemu-guest-agent.service
[Service]
Type=oneshot
ExecStart=/bin/sh -c 'echo "<3>MH-READY $(uname -m) $(cat /proc/uptime | cut -d" " -f1)" > /dev/kmsg'
[Install]
WantedBy=multi-user.target
EOF
systemctl enable mh-ready.service
apt-get clean
if [ $ok = 1 ]; then echo "<3>MHPREP-DONE $(dpkg-query -W -f='${Version}' qemu-guest-agent)" >/dev/kmsg; else echo "<3>MHPREP-FAILED" >/dev/kmsg; fi
sync
systemctl poweroff
"""
UNIT = """[Unit]
Description=Machines harness: first-boot preparation
Wants=network-online.target
After=network-online.target
[Service]
Type=oneshot
ImportCredential=mhprep.sh
ImportCredential=mhprep2.sh
ExecStart=/bin/sh ${CREDENTIALS_DIRECTORY}/mhprep.sh
StandardOutput=journal+console
"""


def b64(s):
    import base64
    return base64.b64encode(s.encode()).decode()


def sha512(path):
    h = hashlib.sha512()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def fetch(url, out):
    req = urllib.request.Request(url, headers={"User-Agent": "lordos-test-harness"})
    with urllib.request.urlopen(req, timeout=60) as r, open(out, "wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)


def debian(arch=ARCH):
    """Debian 13 nocloud for arch, verified; its path."""
    private_dir(IMAGES)
    out = os.path.join(IMAGES, "debian13-%s.qcow2" % arch)
    if os.path.exists(out):
        return out
    name = "debian-13-nocloud-%s.qcow2" % arch
    tmp = tempfile.mkdtemp(prefix=".dl.", dir=IMAGES)
    try:
        try:
            fetch(BASE + "/SHA512SUMS", os.path.join(tmp, "SHA512SUMS"))
            want = [l.split()[0] for l in open(os.path.join(tmp, "SHA512SUMS")) if l.split()[1:] == [name]]
            if not want:
                raise RuntimeError("no %s in Debian's SHA512SUMS" % name)
            log("downloading %s (about 400 MB)" % name)
            fetch(BASE + "/" + name, os.path.join(tmp, name))
        except OSError as e:
            skip("Debian's image couldn't be downloaded: %s" % e)
        if sha512(os.path.join(tmp, name)) != want[0]:
            raise RuntimeError("%s: its SHA-512 isn't Debian's" % name)
        os.chmod(os.path.join(tmp, name), 0o444)
        os.rename(os.path.join(tmp, name), out)
        log("kept %s (SHA-512 as Debian's list)" % out)
        return out
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def catalog():
    return json.load(open(os.path.join(os.path.dirname(BUILD), "..", "assets", "machines", "systems.json")))


def pinned(entry_id, arch="amd64", lang=None):
    for e in catalog().get("systems", []):
        if e.get("id") == entry_id:
            for f in e.get("arch", {}).get(arch, {}).get("pinned", []):
                if lang is None or f.get("lang") == lang:
                    return f
    raise LookupError("no pinned %s %s %s in the catalog" % (entry_id, arch, lang or ""))


def fetch_pinned(f, out):
    """f: the catalog's { url, sha256, bytes }. Resumed when a part is there; kept only when the SHA-256 matches."""
    if os.path.exists(out):
        return out
    private_dir(IMAGES)
    part = out + ".part"
    have = os.path.getsize(part) if os.path.exists(part) else 0
    if have < f["bytes"]:
        req = urllib.request.Request(f["url"], headers={"User-Agent": "lordos-test-harness", "Range": "bytes=%d-" % have})
        try:
            with urllib.request.urlopen(req, timeout=120) as r, open(part, "ab" if have and r.status == 206 else "wb") as o:
                shutil.copyfileobj(r, o, 4 << 20)
        except OSError as e:
            skip("%s couldn't be downloaded: %s" % (f["url"], e))
    if sha256(part) != f["sha256"]:
        os.unlink(part)
        raise RuntimeError("%s: its SHA-256 isn't the pinned one" % os.path.basename(out))
    os.chmod(part, 0o444)
    os.rename(part, out)
    log("kept %s (SHA-256 as pinned in the catalog)" % out)
    return out


def windows_eval(lang="en-US"):
    return fetch_pinned(pinned("windows", "amd64", lang), os.path.join(IMAGES, "win11-eval-%s.iso" % lang.lower()))


def virtio_win():
    f = pinned("virtio-win", "amd64")
    return fetch_pinned(f, os.path.join(IMAGES, f["name"]))


def agent_path(arch=ARCH):
    return os.path.join(IMAGES, "debian13-agent%d-%s.qcow2" % (PREP_VERSION, arch))


def agent_image(arch=ARCH, accel=None):
    """The prepared image (qemu-guest-agent, serial autologin, MH-READY); its path."""
    out = agent_path(arch)
    if os.path.exists(out):
        return out
    native = arch == ARCH
    accel = accel or ("kvm" if native and kvm_ok() else "tcg")
    if accel == "kvm" and not kvm_ok():
        skip("needs /dev/kvm")
    base = debian(arch)
    D = tempfile.mkdtemp(prefix=".prep.", dir=IMAGES)
    os.chmod(D, 0o700)
    for d in ("home", "run", "fake"):
        os.makedirs(os.path.join(D, d), mode=0o700)
    env = {"PATH": "/usr/bin:/bin", "HOME": os.path.join(D, "home"), "XDG_RUNTIME_DIR": os.path.join(D, "run"), "LANG": "C.UTF-8"}
    M = os.path.join(D, "home", "prep.lordvm")
    vm = os.path.join(BUILD, "lord-vm")

    def run(*a, timeout=600):
        return subprocess.run(ns_prefix(D) + [vm] + list(a), env=env, capture_output=True, text=True, timeout=timeout)
    try:
        r = run("--create", M, "--name", "prep", "--arch", arch, "--accel", accel, "--vcpus", "2", "--memory", "1024",
                "--network", "shared", "--display", "none", "--import", base)
        if r.returncode:
            raise RuntimeError("lord-vm --create: %s %s" % (r.stdout, r.stderr))
        creds = os.path.join(D, "creds")
        pw = "mh-" + os.urandom(6).hex()
        with open(creds, "w") as f:
            f.write("firstboot.locale=C.UTF-8\nfirstboot.keymap=us\nfirstboot.timezone=UTC\n")
            f.write("passwd.plaintext-password.root=%s\n" % pw)
            f.write("binary:mhprep.sh=%s\n" % b64(PREP))
            f.write("binary:mhprep2.sh=%s\n" % b64(PREP2))
            f.write("binary:systemd.extra-unit.lordvm-prep.service=%s\n" % b64(UNIT))
            f.write("binary:systemd.unit-dropin.multi-user.target=%s\n" % b64("[Unit]\nWants=lordvm-prep.service\n"))
        os.chmod(creds, 0o600)
        r = run("--start", M, "--detach", "--keep-running", "--credentials-file", creds)
        if r.returncode:
            raise RuntimeError("lord-vm --start: %s %s" % (r.stdout, r.stderr))
        serial = os.path.join(M, "log", "serial.log")
        end = time.time() + (600 if accel == "kvm" else 3600)
        done = None
        while time.time() < end:
            t = open(serial, errors="replace").read() if os.path.exists(serial) else ""
            if "MHPREP-DONE" in t:
                done = t[t.index("MHPREP-DONE"):].split("\n")[0].strip()
            if "MHPREP-FAILED" in t:
                raise RuntimeError("the guest couldn't install qemu-guest-agent (network?): see %s" % serial)
            st = run("--status", M, "--json", timeout=30).stdout
            if done and '"state":"stopped"' in st.replace(" ", ""):
                break
            time.sleep(2)
        else:
            raise RuntimeError("the preparation didn't finish in time; serial log: %s" % serial)
        log("prepared: %s" % done)
        tmp = out + ".tmp"
        subprocess.run(["qemu-img", "convert", "-O", "qcow2", "-c", os.path.join(M, "disks", "system.qcow2"), tmp], check=True)
        os.chmod(tmp, 0o444)
        os.rename(tmp, out)
        with open(out + ".sha256", "w") as f:
            f.write("%s  %s\n" % (sha256(out), os.path.basename(out)))
        return out
    finally:
        run("--poweroff", M, timeout=60)
        if os.environ.get("MH_KEEP") != "1":
            shutil.rmtree(D, ignore_errors=True)


if __name__ == "__main__":
    a = sys.argv[1:]
    arch = a[a.index("--arch") + 1] if "--arch" in a else ARCH
    try:
        if "--windows" in a:
            print(windows_eval(a[a.index("--lang") + 1] if "--lang" in a else "en-US"))
            print(virtio_win())
            sys.exit(0)
        print(agent_image(arch) if "--agent" in a else debian(arch))
    except Skip as e:
        print("skipped: %s" % e)
        sys.exit(77)
