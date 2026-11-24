# nolandlock_inner.py — run by nolandlock.py inside its guest (a kernel without Landlock), as the guest's person: the
# Machines app in a headless session (mh.py), its "Weaker isolation" notice, and a machine started there (emulated).
import json, os, sys, time
sys.path.insert(0, "/mnt/src/tools/vm/e2e/machines")
import re
import mh
import flows
r = mh.Results("weaker isolation (no Landlock)")
lsm = open("/sys/kernel/security/lsm").read().strip()
r.t("this kernel runs without Landlock (%s)" % lsm, "landlock" not in lsm.split(","))
with mh.Session(name="nll") as s:
    h = s.js("return lord.vm.host()")
    print("HOST", json.dumps({k: h.get(k) for k in ("landlock", "kvm", "arch")}))
    r.t("lord.vm.host().landlock is 0", h.get("landlock") == 0)
    s.js("launchApp('machines')")
    w = s.window_rect("machines")
    s.wait_text(re.compile(r"^Weaker isolation on this kernel\."), 20)
    s.shot("library-weaker")
    r.ok("the library's notice: Weaker isolation on this kernel")
    M = os.path.join(s.library, "Plain.lordvm")
    s.lordvm("--create", M, "--name", "Plain", "--accel", "tcg", "--memory", "512", "--disk", "1", "--display", "window")
    s.wait_js("lord.vm.list().some(m => m.name === 'Plain')", 20)
    flows.card_menu(s, "Plain", "Configure…")
    w = flows.lib_rect(s)
    s.wait_text("Startup and shutdown", 10, w)
    s.click_text("Security", within=w)
    s.wait_text(re.compile(r"^This kernel has no Landlock"), 10, w)
    s.shot("configure-security-weaker")
    r.ok("Configure → Security: This kernel has no Landlock … Weaker isolation.")
    s.click_text("Done", within=w)
    time.sleep(1)
    rs = s.lordvm("--start", M, "--detach", "--keep-running", check=False)
    print("START", rs.stdout.strip(), rs.stderr.strip()[-300:])
    try:
        s.wait_state("Plain", "running", 120)
    except TimeoutError:
        print("VMLOG-TAIL", "\n".join(s.vmlog(M).splitlines()[-15:]))
        raise
    time.sleep(3)
    st = s.status(M)
    print("STATUS", json.dumps(st.get("sandbox_detail") or st))
    s.shot("machine-running")
    # the machine's window (a double click on its card), then its Info menu
    try:
        s.js("wm.focus('machines')")
        s.click_text("Plain", within=flows.lib_rect(s), n=2)
        time.sleep(4)
        flows.allow_shortcuts(s)
        time.sleep(1)
        s.click_text("Info")
        s.wait_text("Sandbox: weaker isolation on this kernel", 10)
        s.shot("window-info-weaker")
        r.ok("the window's Info menu: Sandbox: weaker isolation on this kernel")
    except Exception as e:
        s.shot("window-info-failed")
        r.bad("the window's Info menu", str(e)[:300])
    log = s.vmlog(M)
    ll = [l for l in log.splitlines() if "andlock" in l or "sandbox" in l.lower()]
    print("VMLOG", "\n".join(ll[-8:]))
    r.t("lord-vm's log says Landlock isn't there", any("andlock" in l for l in ll))
    s.lordvm("--poweroff", M, check=False)
    r.t("no JS error", not s.errors(), "; ".join(s.errors()[:3]))
sys.exit(r.end())
