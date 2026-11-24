# perf_linux.py — the guest-side performance profile in a running Linux guest (docs/VIRTUALIZATION.md 7.1.4, `lordos-guest`
# 7): the harness's Debian 13 guest (images.py) with Debian's real power-profiles-daemon on its system bus and GNOME's
# settings in the real dconf (gsettings-desktop-schemas, dconf-service) on a session bus, this build's lordos-guest on the
# machine's org.lordos.agent port, the machine's "performance profile" on (lord-vm --set --perf-profile on), started by
# this build's lord-vm in a scratch library (private namespaces, like boot-matrix.py). Checked: the power profile goes to
# performance and the animations off while the tools run, lord-vm hears it (AGK_PERFSTATE in its log), and both are put
# back when the tools stop (SIGTERM). Not a GNOME desktop: the session bus is dbus-run-session's, XDG_CURRENT_DESKTOP set.
# Test only.   python3 -I tools/vm/e2e/machines/perf_linux.py
import base64, hashlib, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mh  # noqa: E402
from targets import Lib  # noqa: E402

AGENT = os.path.join(mh.BUILD, "lordos-guest")


def main():
    if not mh.kvm_ok():
        print("skipped: no /dev/kvm"); return 77
    r = mh.Results("performance profile in a running Linux guest")
    L = Lib()
    try:
        M = L.make("perf", "--accel", "kvm", "--vcpus", "2", "--memory", "2048")
        rr = L.vm("--set", M, "--perf-profile", "on")
        r.t("--perf-profile on", rr.returncode == 0, rr.stdout + rr.stderr)
        L.boot(M)
        con = L.console(M)
        o, _ = con.run("DEBIAN_FRONTEND=noninteractive apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "
                       "power-profiles-daemon gsettings-desktop-schemas dconf-service dconf-gsettings-backend dbus-user-session "
                       "libglib2.0-bin >/tmp/apt.log 2>&1; echo APT=$?; systemctl start power-profiles-daemon; sleep 2; "
                       "powerprofilesctl get", 1500)
        print(o.strip()[-200:])
        before = o.strip().splitlines()[-1] if o.strip() else ""
        r.t("power-profiles-daemon runs (%s)" % before, before in ("balanced", "power-saver", "performance"))
        b64 = base64.b64encode(open(AGENT, "rb").read()).decode()
        con.run("rm -f /tmp/lg.b64", 10)
        for i in range(0, len(b64), 3000):
            con.run("printf %%s '%s' >> /tmp/lg.b64" % b64[i:i + 3000], 20)
        o, _ = con.run("base64 -d /tmp/lg.b64 > /usr/local/bin/lordos-guest && chmod 755 /usr/local/bin/lordos-guest && "
                       "sha256sum /usr/local/bin/lordos-guest; ls -la /dev/virtio-ports/", 30)
        r.t("this build's lordos-guest in the guest", hashlib.sha256(open(AGENT, "rb").read()).hexdigest() in o, o)
        anim0, _ = con.run("dbus-run-session -- gsettings get org.gnome.desktop.interface enable-animations", 30)
        # the tools, as a desktop session would run them: a session bus (dconf), GNOME's name
        con.run("(XDG_CURRENT_DESKTOP=GNOME setsid dbus-run-session -- /usr/local/bin/lordos-guest > /tmp/lg.log 2>&1 &) ; sleep 6", 30)
        o, _ = con.run("powerprofilesctl get; dbus-run-session -- gsettings get org.gnome.desktop.interface enable-animations; "
                       "cat /tmp/lg.log", 30)
        print(o)
        lines = o.strip().splitlines()
        r.t("while the tools run: performance (%s)" % (lines[0] if lines else "?"), bool(lines) and lines[0].strip() == "performance")
        r.t("while the tools run: animations off (was %s)" % anim0.strip(), len(lines) > 1 and lines[1].strip() == "false")
        time.sleep(2)
        lg = [l for l in open(os.path.join(M, "log", "lord-vm.log"), errors="replace").read().splitlines() if "erformance" in l or "perf" in l]
        print("   | " + "\n   | ".join(lg[-4:]))
        r.t("lord-vm hears the guest's profile", any("performance profile" in l for l in lg))
        con.run("pkill -TERM -x lordos-guest; sleep 3", 20)
        o, _ = con.run("powerprofilesctl get; dbus-run-session -- gsettings get org.gnome.desktop.interface enable-animations; tail -3 /tmp/lg.log", 30)
        print(o)
        lines = o.strip().splitlines()
        r.t("the tools stopped: back to %s (%s)" % (before, lines[0] if lines else "?"), bool(lines) and lines[0].strip() == before)
        r.t("the tools stopped: animations back (%s)" % (lines[1] if len(lines) > 1 else "?"), len(lines) > 1 and lines[1].strip() == anim0.strip())
        con.close()
        L.off(M)
    finally:
        L.close()
    return r.end()


if __name__ == "__main__":
    mh.main_guard(main)
