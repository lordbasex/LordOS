# stop3d_session.py — stop3d.py's checks with the machine's window open in a headless session on the GPU path
# (mh.Session(gpu=True): lord-server and lord-vmdisplay take the 3D picture as a dmabuf, as on a desktop), which is when
# QEMU 10.0.13 aborts at a 3D guest's shutdown or restart (QEMU issue 4918; without a display client stop3d.py saw it
# end cleanly). The harness's Debian 13 guest (Debian's 6.12.118 kernel), 3D on the render node lord-server uses.
#   1. no guest tools, a power-off after 30 s       → stopped, qemu-3d-stop (or guest-shutdown when QEMU didn't abort)
#   2. this build's lordos-guest and its hook: a restart after 30 s → stopped, reset, and the window starts it again;
#      then a power-off after 30 s → stopped, guest-shutdown; the next start checks the disks
# Test only.   python3 -I tools/vm/e2e/machines/stop3d_session.py
import base64, hashlib, json, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mh  # noqa: E402
import images  # noqa: E402
import flows  # noqa: E402

AGENT = os.path.join(mh.BUILD, "lordos-guest")
NAME = "Three D"


def main():
    if not mh.kvm_ok() or not any(n.startswith("renderD") for n in os.listdir("/dev/dri")):
        mh.skip("needs KVM and a render node")
    r = mh.Results("a 3D guest's stop, its window open (GPU path)")
    with mh.Session(gpu=True, name="s3d") as s:
        M = os.path.join(s.library, NAME + ".lordvm")
        s.lordvm("--create", M, "--name", NAME, "--accel", "kvm", "--graphics", "3d", "--vcpus", "2", "--memory", "4096",
                 "--agent", "on", "--display", "window", "--import", images.agent_image(images.ARCH), timeout=300)
        mid = json.load(open(os.path.join(M, "machine.json")))["id"]
        s.js("launchApp('machines')")
        time.sleep(1.5)
        flows.lib_rect(s)

        def start_from_card(n_ready):
            s.js("wm.focus('machines')")
            time.sleep(0.3)
            s.click_text(NAME, within=flows.lib_rect(s), n=2)
            s.wait_state(NAME, "running", 90)
            flows.allow_shortcuts(s, 5)
            end = time.time() + 180
            while time.time() < end and s.serial(M).count("MH-READY") < n_ready:
                time.sleep(1)

        def end_state(t=150):
            end = time.time() + t
            while time.time() < end:
                st = s.status(M)
                if st.get("state") in ("stopped", "crashed"):
                    return st
                time.sleep(0.5)
            return s.status(M)

        def logl(pat, n=6):
            return [l[:220] for l in s.vmlog(M).splitlines() if re.search(pat, l)][-n:]

        start_from_card(1)
        r.t("3D is on (virtio-vga-gl)", any("virtio-vga-gl" in l for l in s.vmlog(M).splitlines()))
        s.shot("3d-running")
        con = s.console(mid)
        k, _ = con.run("uname -r", 20)
        print("guest kernel:", k.strip().splitlines()[-1] if k.strip() else "?")
        time.sleep(35)
        con.run("systemctl poweroff", 5)
        con.close()
        st = end_state()
        print("1. no tools, power off: %s %s code %s" % (st.get("state"), st.get("reason"), st.get("code")))
        for l in logl(r"3D|abort|qemu: exited|signal|state stopped|state crashed"):
            print("   | " + l)
        s.shot("3d-1-after-poweroff")
        r.t("1. no tools: stopped, qemu-3d-stop or guest-shutdown (%s)" % st.get("reason"),
            st.get("state") == "stopped" and st.get("reason") in ("qemu-3d-stop", "guest-shutdown"))
        # 2. the tools and their hook
        start_from_card(1)
        con = s.console(mid)
        b64 = base64.b64encode(open(AGENT, "rb").read()).decode()
        con.run("rm -f /tmp/lg.b64", 10)
        for i in range(0, len(b64), 3000):
            con.run("printf %%s '%s' >> /tmp/lg.b64" % b64[i:i + 3000], 20)
        o, _ = con.run("base64 -d /tmp/lg.b64 > /usr/local/bin/lordos-guest && chmod 755 /usr/local/bin/lordos-guest && "
                       "install -d -m 755 /usr/lib/systemd/system-shutdown && printf '%s\\n' '#!/bin/sh' "
                       "'exec /usr/local/bin/lordos-guest --stopping \"$1\"' > /usr/lib/systemd/system-shutdown/lordos-guest-local && "
                       "chmod 755 /usr/lib/systemd/system-shutdown/lordos-guest-local && sha256sum /usr/local/bin/lordos-guest", 60)
        r.t("lordos-guest and its hook are in the guest", hashlib.sha256(open(AGENT, "rb").read()).hexdigest() in o)
        time.sleep(35)
        con.run("systemctl reboot", 5)
        con.close()
        st = end_state()
        print("2a. tools, restart: %s %s code %s" % (st.get("state"), st.get("reason"), st.get("code")))
        for l in logl(r"3D|abort|stopping|qemu: exited|state stopped|state crashed"):
            print("   | " + l)
        r.t("2a. tools, restart: stopped, reset (%s)" % st.get("reason"), st.get("state") == "stopped" and st.get("reason") == "reset")
        # the window starts it again by itself (restartsAgain)
        try:
            s.wait_state(NAME, "running", 60)
            r.ok("the window started it again after the restart")
        except TimeoutError as e:
            r.bad("the window started it again after the restart", str(e)[:200])
        end = time.time() + 180
        while time.time() < end and "MH-READY" not in s.serial(M):
            time.sleep(1)
        print("disk check:", logl(r"qemu-img|leak|check", 3))
        time.sleep(35)
        con = s.console(mid)
        con.run("systemctl poweroff", 5)
        con.close()
        st = end_state()
        print("2b. tools, power off: %s %s code %s" % (st.get("state"), st.get("reason"), st.get("code")))
        for l in logl(r"3D|abort|stopping|qemu: exited|state stopped|state crashed"):
            print("   | " + l)
        s.shot("3d-2b-after-poweroff")
        r.t("2b. tools, power off: stopped, guest-shutdown (%s)" % st.get("reason"),
            st.get("state") == "stopped" and st.get("reason") == "guest-shutdown")
        r.t("no JS error", not s.errors(), "; ".join(s.errors()[:3]))
    return r.end()


mh.main_guard(main)
