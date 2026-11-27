# flows.py — the Machines app's end-to-end flows (README.md here), each a function flow_<name>(s, r, c): s the Session
# (mh.py), r the Results, c a dict the flows share (the machine they made, its folder, its id). They run in the order of
# FLOWS in one session and one scratch library; a flow that needs what an earlier one made skips when it isn't there.
# Every step looks at the app as a person would (texts on screen, a click where they are drawn) and at what is on disk or
# in lord-vm, and keeps a screenshot (MH_OUT, or the run's shots/ folder).
import json, os, platform, re, shutil, subprocess, time
from mh import Session, Image, BUILD, IMAGES, skip, log, kvm_ok, have
import images

# Create and Start copies the image into the machine's folder first: 120 s on an SSD, up to 480 s on a Raspberry Pi's SD
# card (ws171, 54 of 54 passed there with 480). MH_CREATE_S overrides it; the default is longer on arm64.
CREATE_S = int(os.environ.get("MH_CREATE_S") or (480 if platform.machine() in ("aarch64", "arm64") else 120))

NAME = "MH Debian"
CLONE = "MH Debian clone"


def lib_rect(s):
    """The Control Center's rectangle, opened (and brought to the front) as the Dock's Machines item does."""
    if not any(w["id"] == "machines" and not w.get("minimized") for w in s.windows()):
        s.js("launchApp('machines')")
    s.js("wm.focus('machines')")
    time.sleep(0.3)
    w = s.window("machines", lambda w: w["id"] == "machines")
    if w["h"] < 640:
        # its sheets fit its first size (760×500: their bodies scroll, tests/machines_sheets_headless.sh), but Configure's
        # rows the flows click are all on screen only in a bigger window: it is made bigger first, as a person would; the
        # new size reaches wm.windows() a moment later
        s.js("wm.setFrame('machines', { x: 40, y: 36, w: 1200, h: 680 })")
        end = time.time() + 5
        while w["h"] < 640 and time.time() < end:
            time.sleep(0.3)
            w = s.window("machines", lambda w: w["id"] == "machines")
    return s.rect_of(w)


def vm_window(s, name, timeout=20):
    return s.window("machines", lambda w: w["title"].startswith(name + " —"), timeout=timeout)


def allow_shortcuts(s, wait=0):
    """The first machine window asks for the keyboard shortcuts while it's in front (Privacy): allowed, as a person would.
    The question covers the whole screen (its scrim), so every flow answers it before it clicks anything else."""
    end = time.time() + wait
    while True:
        if s.find(re.compile(r"wants to use the keyboard shortcuts")):
            s.click_text("Allow")
            time.sleep(0.5)
            return True
        if time.time() >= end:
            return False
        time.sleep(0.3)


def menu(s, item, *path):
    """A menu bar item, then its entries: menu(s, 'Actions', 'Shut Down')."""
    allow_shortcuts(s)
    s.click_text(item, within=[0, 0, s.W, 28])
    time.sleep(0.4)
    for p in path:
        s.click_text(p)
        time.sleep(0.4)


def card_menu(s, name, entry):
    """The library card's context menu (a right click on its name), then one entry."""
    allow_shortcuts(s)
    s.js("wm.focus('machines')")
    time.sleep(0.3)
    s.click_text(name, within=lib_rect(s), button=2)
    time.sleep(0.5)
    s.click_text(entry)
    time.sleep(0.8)


def need_image(c):
    img = images.agent_path(c["arch"])
    if not os.path.exists(img):
        skip("the guest image isn't made (python3 -I tools/vm/e2e/machines/images.py --agent)")
    return img


def need_machine(s, c, running=None):
    if "dir" not in c:
        skip("no machine (the assistant flow didn't make one)")
    if running is not None:
        st = s.vm(NAME).get("state")
        if running and st != "running":
            skip("%s isn't running (%s)" % (NAME, st))
        if not running and st != "stopped":
            power_off_ui(s, c)


# ---------------------------------------------------------------- the flows

def flow_premade(s, r, c):
    """Not one of the default flows: the machine the assistant would make, made by lord-vm directly (the same image, 2
    vCPUs, 4 GB, shared network, a window), so a later flow runs alone: run.py premade snapshots."""
    if not kvm_ok():
        skip("needs /dev/kvm")
    img = need_image(c)
    M = os.path.join(s.library, NAME + ".lordvm")
    s.lordvm("--create", M, "--name", NAME, "--vcpus", "2", "--memory", "4096", "--display", "window", "--import", img, timeout=300)
    m = s.vm(NAME, timeout=20)
    c.update(dir=m["dir"], id=m["id"])
    r.ok("premade: %s made by lord-vm" % NAME)


def flow_library(s, r, c):
    s.js("launchApp('machines')")
    w = s.window("machines")
    r.step("library: the Control Center opens", lambda: s.wait_text("No machines yet", 15, s.rect_of(w)))
    img = s.shot("01-library-empty")
    r.t("library: its window is drawn (not a flat fill)", img.varied(s.rect_of(w), 40))
    r.t("library: New Machine and Import are offered", bool(s.find("New Machine", s.rect_of(w))) and bool(s.find("Import a machine", s.rect_of(w))))


def flow_assistant(s, r, c):
    """New Machine → Import a machine → the image in the file picker → Light profile → a name → Create and Start."""
    if not kvm_ok():
        skip("needs /dev/kvm")
    need_image(c)
    if not os.path.exists(os.path.join(s.home, "Images", "debian13.qcow2")):
        skip("the session wasn't given the image (run.py puts it in ~/Images)")
    w = lib_rect(s)
    s.click_text("New Machine", within=w)
    s.wait_text(re.compile(r"Step 1 of \d · Choose"), 10)
    s.shot("02-assistant-choose")
    sheet = s.find("What do you want to run?")[0]["rect"]
    for _ in range(4):                              # "Or bring your own" is under the download cards
        if s.find("Import a machine", [w[0], sheet[1], w[2], w[3] - (sheet[1] - w[1])]):
            break
        s.remote.wheel(sheet[0] + 300, sheet[1] + 150, -4)
        time.sleep(0.4)
    s.click_text("Import a machine", within=[w[0], sheet[1], w[2], w[3]])
    s.wait_text("Choose a disk image", 10)
    s.shot("03-assistant-picker")
    s.click_text("Images", n=2)
    s.click_text("debian13.qcow2")
    s.click_text("Open")
    r.step("assistant: the image is chosen", lambda: s.wait_text("debian13.qcow2", 10))
    s.click_text("Continue")
    s.wait_text(re.compile(r"Step 2 of \d · Profile"), 10)
    s.click_text("Light")
    s.shot("04-assistant-profile")
    s.click_text("Continue")
    s.wait_text(re.compile(r"Step 3 of \d · Name"), 10)
    # the name field holds a suggestion: selected and replaced
    field = s.find("Name", w)
    s.press("ctrl", "a")
    s.type(NAME)
    time.sleep(0.3)
    if not s.find(NAME):
        # the field didn't have the keyboard: click into it (under its label) and type again
        x, y, ww, hh = field[0]["rect"]
        s.click(x + 60, y + hh + 18)
        s.press("ctrl", "a")
        s.type(NAME)
    r.step("assistant: the name is typed", lambda: s.wait_text(NAME, 5))
    s.shot("05-assistant-name")
    s.click_text("Continue")
    s.wait_text("Ready to create", 10)
    review = s.texts(lib_rect(s))
    s.shot("06-assistant-review")
    r.t("assistant: the review names the image and the Light profile's 2 vCPUs", "debian13.qcow2" in review and any("2 vCPU" in t for t in review), review)
    t0 = time.time()
    s.click_text("Create and Start")
    m = r.step("assistant: Create and Start makes the machine and it runs", lambda: s.wait_state(NAME, "running", CREATE_S))
    if not m:
        return
    c.update(dir=m["dir"], id=m["id"], created_s=round(time.time() - t0, 1))
    mj = json.load(open(os.path.join(m["dir"], "machine.json")))
    # an imported image of an unknown system starts with the guest agent off (Configure turns it on: flow_configure)
    r.t("assistant: machine.json has the name, the Light profile's 2 vCPUs and 4 GB", mj.get("name") == NAME and mj.get("vcpus") == 2 and mj.get("memory_mb") == 4096, mj)
    vw = r.step("assistant: the machine's window opens", lambda: vm_window(s, NAME))
    allow_shortcuts(s, 8)
    ready = r.step("guest: Debian boots to MH-READY on its serial console", lambda: s.wait_serial(m["dir"], "MH-READY", 180))
    c["boot_s"] = round(time.time() - t0, 1)
    if vw and ready:
        vr = s.rect_of(vm_window(s, NAME))
        guest = [vr[0] + 8, vr[1] + 100, vr[2] - 16, vr[3] - 110]
        # the console's login prompt (getty clears the screen first): a few seconds after the guest said it's up
        end = time.time() + 30
        while True:
            img = s.shot("07-machine-window")
            if img.ink(guest) > 0.001 or time.time() > end:
                break
            time.sleep(2)
        r.t("window: the guest's picture is drawn (its console's text, not a flat fill)", img.ink(guest) > 0.001, img.ink(guest))


def flow_agent(s, r, c):
    """qemu-guest-agent connects; the agent popover lists what works; its Install Clipboard Helper (a Linux guest; Install
    LordOS Tools on Windows) puts lordos-guest in the guest through the agent (7.1), and it says hello."""
    need_machine(s, c, running=True)
    r.step("agent: qemu-guest-agent connects (lord.vm.list agentState)", lambda: s.wait_js(
        "lord.vm.list().find((m) => m.name === %s)?.agentState === 'connected'" % json.dumps(NAME), 90))
    allow_shortcuts(s)
    s.js("wm.focus(%s)" % json.dumps(vm_window(s, NAME)["id"]))
    vr = s.rect_of(vm_window(s, NAME))
    s.click_icon("agent", within=vr)                # the toolbar's agent button (an icon)
    r.step("agent: the popover says connected", lambda: s.wait_text("Agent connected", 10))
    rows = s.texts(vr)
    s.shot("08-agent-popover")
    works = sum(1 for t in rows if t == "Works")
    r.t("agent: the popover lists what works through it (shell, logs, shutdown, clock, folders…)", works >= 5 and
        any("Shell" in t for t in rows), rows)
    inst = s.find(re.compile(r"^Install (Clipboard Helper|LordOS Tools)$"), vr)
    if not inst:
        s.press("esc")
        skip("no install button in the popover (texts: %s)" % rows[:30])
    x, y, w, h = inst[0]["rect"]
    s.click(x + w // 2, y + h // 2)
    # the clipboard helper starts in the guest's desktop session: this console-only guest has none, so it is installed
    # and not running (the helper itself is tests/guest_clip_test.sh's)
    ok = r.step("agent: lord-vm installs lordos-guest through the agent", lambda: _wait_log(s, c["dir"], re.compile(r"agent: lordos-guest installed"), 120))
    s.shot("08-agent-installed")
    s.click(vr[0] + vr[2] - 60, vr[1] + vr[3] - 40)       # away from the popover: it closes (no key reaches the guest)
    if ok:
        con = s.console(c["id"])
        out, st = con.run("for f in /usr/local/bin/lordos-guest /usr/bin/lordos-guest; do [ -x $f ] && $f --version 2>&1 | head -1 && echo $f; done")
        con.close()
        r.t("agent: lordos-guest is in the guest and runs (%s)" % out.replace("\n", " "), "lordos-guest" in out, out)


def flow_clipboard(s, r, c):
    """The clipboard both ways (6.8), through lordos-guest and the guest's xclip on its Xvfb display: a text copied here
    is pasted in the machine while its window is in use, and a text copied in the machine reaches this computer's
    clipboard. Needs the agent flow's Install Clipboard Helper and Configure's Both ways."""
    need_machine(s, c, running=True)
    if '"clipboard": "both"' not in open(os.path.join(c["dir"], "machine.json")).read():
        skip("the machine's clipboard isn't Both ways (the configure flow)")
    con = s.console(c["id"])
    out, st = con.run("command -v xclip Xvfb; pgrep -a Xvfb; [ -x /usr/local/bin/lordos-guest ] && echo HELPER")
    if "HELPER" not in out or "xclip" not in out:
        con.close()
        skip("the guest has no lordos-guest or xclip (the agent flow, the image's version): %s" % out)
    # the helper, as the guest's desktop session would start it (root here: the console's session has no seat)
    con.run("pkill -x lordos-guest; systemctl stop mh-lg 2>/dev/null; systemd-run --unit mh-lg -E DISPLAY=:0 /usr/local/bin/lordos-guest")
    r.step("clipboard: lordos-guest says hello with a clipboard tool", lambda: _wait_log(s, c["dir"], re.compile(r"agent: lordos-guest \S+ said hello"), 30))
    con.run("pkill -x xclip")                                 # no copy of the guest's own owns the clipboard there
    # copied here (another window in front), then the person turns to the machine: a click on its picture (6.8: the
    # offer goes when the window comes to the front or its picture is clicked)
    lib_rect(s)
    word = "mh-host-%s" % os.urandom(3).hex()
    s.js("sdk.clipboard.set(%s)" % json.dumps(word))
    time.sleep(0.5)
    vw = vm_window(s, NAME)
    s.js("wm.focus(%s)" % json.dumps(vw["id"]))
    vr = s.rect_of(vw)
    s.click(vr[0] + vr[2] // 2, vr[1] + vr[3] // 2)
    got = None
    end = time.time() + 30
    while time.time() < end:
        out, st = con.run("DISPLAY=:0 timeout 5 xclip -o -selection clipboard -t UTF8_STRING 2>&1")
        if word in out:
            got = out
            break
        time.sleep(1)
    r.t("clipboard: a text copied here is pasted in the machine (%s)" % word, got is not None, out)
    back = "mh-guest-%s" % os.urandom(3).hex()
    con.run("printf %s %s | DISPLAY=:0 xclip -i -selection clipboard" % ("%s", back))
    end = time.time() + 30
    host = None
    while time.time() < end:
        host = s.js("return lord.clipboard.get ? lord.clipboard.get() : null")
        if isinstance(host, str) and back in host:
            break
        time.sleep(1)
    r.t("clipboard: a text copied in the machine reaches this computer's clipboard (%s)" % back, isinstance(host, str) and back in host, host)
    s.shot("12-clipboard")
    con.close()


def _wait_log(s, mdir, rx, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if rx.search(s.vmlog(mdir)):
            return True
        time.sleep(1)
    raise TimeoutError("no %s in lord-vm.log" % rx.pattern)


def flow_filedrop(s, r, c):
    """A file of this computer dragged from Files onto the machine's window lands in the guest's Downloads (6.21)."""
    need_machine(s, c, running=True)
    src = os.path.join(s.home, "Documents", "mh-drop.txt")
    os.makedirs(os.path.dirname(src), exist_ok=True)
    body = "dropped by the Machines harness %s\n" % os.urandom(4).hex()
    open(src, "w").write(body)
    s.js("launchApp('files', { path: %s })" % json.dumps(os.path.dirname(src)))
    fw = s.window("files", timeout=15)
    s.js("wm.move(%s, 40, 420)" % json.dumps(fw["id"]))
    time.sleep(0.8)
    if not s.find("mh-drop.txt", s.rect_of(s.window("files"))):
        s.click_text("Documents", within=s.rect_of(s.window("files")))
    t = s.wait_text("mh-drop.txt", 10, s.rect_of(s.window("files")))
    vr = s.rect_of(vm_window(s, NAME))
    x, y, w, h = t["rect"]
    # held on the file's icon (above its name: a press on the name selects it for renaming), carried to the machine
    x0, y0, x1, y1 = x + w // 2, y - 30, vr[0] + vr[2] // 2, vr[1] + 250
    s.remote.move(x0, y0); time.sleep(0.2); s.remote.button(0, 1); time.sleep(0.3)
    for i in range(1, 31):
        s.remote.move(x0 + (x1 - x0) * i / 30, y0 + (y1 - y0) * i / 30); time.sleep(0.04)
    time.sleep(0.5)
    over = s.texts(vr)
    s.shot("09-filedrop-over")
    s.remote.button(0, 0)
    r.t("filedrop: the machine's window takes the drop (no 'needs the guest agent')", not any("needs the guest agent" in x for x in over), over)
    con = s.console(c["id"])
    got = out = None
    end = time.time() + 90
    while time.time() < end:
        out, st = con.run("find / -xdev -name mh-drop.txt 2>/dev/null | head -3 | while read f; do echo \"$f\"; cat \"$f\"; done")
        if body.strip() in out:
            got = out
            break
        time.sleep(3)
    con.close()
    s.shot("09-filedrop")
    c["drop_path"] = got.split("\n")[0] if got else None
    r.t("filedrop: the file is in the guest (%s), its bytes as on the host" % (c["drop_path"] or "-"), got is not None, out)
    s.js("wm.close(%s)" % json.dumps(s.window("files")["id"]))


def _drag_file(s, name, vr):
    """Files' icon of NAME (in the Documents window already open) held and carried onto the machine's window."""
    t = s.wait_text(name, 10, s.rect_of(s.window("files")))
    x, y, w, h = t["rect"]
    x0, y0, x1, y1 = x + w // 2, y - 30, vr[0] + vr[2] // 2, vr[1] + 250
    s.remote.move(x0, y0); time.sleep(0.2); s.remote.button(0, 1); time.sleep(0.3)
    for i in range(1, 31):
        s.remote.move(x0 + (x1 - x0) * i / 30, y0 + (y1 - y0) * i / 30); time.sleep(0.04)
    time.sleep(0.5)
    s.remote.button(0, 0)


def flow_killcopy(s, r, c):
    """Agent kill test mid-transfer (12, "Agent"): a 300 MB file dropped on the machine, and the guest agent killed in the
    guest while it is being copied in. Nothing partial may appear in the guest's Downloads: the file is there whole (the
    copy finished first) or not at all, and the window says what happened."""
    need_machine(s, c, running=True)
    s.wait_js("lord.vm.list().find((m) => m.name === %s)?.agentState === 'connected'" % json.dumps(NAME), 90)
    src = os.path.join(s.home, "Documents", "mh-big.bin")
    os.makedirs(os.path.dirname(src), exist_ok=True)
    with open(src, "wb") as f:
        for _ in range(300):
            f.write(os.urandom(1 << 20))
    import hashlib
    want = hashlib.sha256(open(src, "rb").read()).hexdigest()
    con = s.console(c["id"])
    s.js("launchApp('files')")
    fw = s.window("files", timeout=15)
    s.js("wm.move(%s, 40, 420)" % json.dumps(fw["id"]))
    time.sleep(0.8)
    if not s.find("mh-big.bin", s.rect_of(s.window("files"))):
        s.click_text("Documents", within=s.rect_of(s.window("files")))
    n0 = s.vmlog(c["dir"]).count("files: copying mh-big.bin")
    _drag_file(s, "mh-big.bin", s.rect_of(vm_window(s, NAME)))
    r.step("killcopy: the copy starts", lambda: _until(lambda: s.vmlog(c["dir"]).count("files: copying mh-big.bin") > n0, 30, 0.1))
    con.run("systemctl kill -s KILL qemu-guest-agent")
    t_kill = time.time()
    s.shot("13-killcopy")
    # the outcome: lord-vm says the file was copied, or that it wasn't
    rx = re.compile(r"files: mh-big\.bin (wasn't copied.*|is in the guest.*|copied.*)")
    _until(lambda: rx.search(s.vmlog(c["dir"]).split("files: copying mh-big.bin")[-1]), 120, 1)
    said = rx.search(s.vmlog(c["dir"]).split("files: copying mh-big.bin")[-1]).group(0)
    c["killcopy"] = {"said": said, "s_after_kill": round(time.time() - t_kill, 1)}
    con.run("systemctl start qemu-guest-agent")
    out, st = con.run("for f in /home/tester/Downloads/*mh-big* /home/tester/Downloads/.*mh-big*; do [ -e \"$f\" ] && sha256sum \"$f\"; done; echo END", 120)
    con.close()
    files = [l.split() for l in out.splitlines() if "mh-big" in l]
    bad = [f for f in files if len(f) < 2 or f[0] != want or not f[1].endswith("/mh-big.bin")]
    r.t("killcopy: nothing partial in the guest's Downloads (%s)" % ("the whole file" if files and not bad else "no file" if not files else "PARTIAL"),
        not bad, out)
    r.t("killcopy: lord-vm says what happened (%s)" % said[:80], bool(said))
    r.step("killcopy: the agent connects again", lambda: s.wait_js(
        "lord.vm.list().find((m) => m.name === %s)?.agentState === 'connected'" % json.dumps(NAME), 90))
    s.js("wm.close(%s)" % json.dumps(s.window("files")["id"]))
    os.unlink(src)


def flow_lifecycle(s, r, c):
    """Actions → Shut Down (the power button: ACPI, or the agent when it's on) → Off."""
    need_machine(s, c, running=True)
    s.js("wm.focus(%s)" % json.dumps(vm_window(s, NAME)["id"]))
    menu(s, "Actions", "Shut Down")
    m = r.step("lifecycle: Shut Down turns it off by itself", lambda: s.wait_state(NAME, "stopped", 120))
    if m:
        lg = s.vmlog(c["dir"])
        # VR_HOST_SHUTDOWN: asked by Machines and obeyed by the guest (vmproto.h); without the agent, the power button
        r.t("lifecycle: the guest obeyed (the power button, then its own shutdown; reason host-shutdown)",
            "qemu: power button pressed" in lg and "SHUTDOWN guest=1 reason=guest-shutdown" in lg and m.get("reason") == "host-shutdown",
            (m.get("reason"), lg.splitlines()[-4:]))
    s.shot("10-shut-down")


def flow_start(s, r, c):
    """A double click on the library card starts the machine in its window; it boots again (serial.log starts over at
    each start)."""
    need_machine(s, c, running=False)
    n = 0
    s.js("wm.focus('machines')")
    time.sleep(0.3)
    s.click_text(NAME, within=lib_rect(s), n=2)
    r.step("start: a double click on its card starts it", lambda: s.wait_state(NAME, "running", 60))
    allow_shortcuts(s, 5)
    r.step("start: it boots again", lambda: _wait_count(s, c["dir"], "MH-READY", n + 1, 150))
    s.shot("11-started-again")


def power_off_ui(s, c):
    """Actions → Force Off in the machine's window (asked to confirm when it asks)."""
    if s.vm(NAME).get("state") == "stopped":
        return
    s.js("wm.focus(%s)" % json.dumps(vm_window(s, NAME)["id"]))
    menu(s, "Actions", "Force Off")
    time.sleep(0.5)
    if s.find("Force Off"):
        s.click_text("Force Off")
    s.wait_state(NAME, "stopped", 60)


def _wait_count(s, mdir, text, n, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if s.serial(mdir).count(text) >= n:
            return True
        time.sleep(1)
    raise TimeoutError("%r seen %d times, not %d" % (text, s.serial(mdir).count(text), n))


def flow_kills(s, r, c):
    """Agent kill tests (12): the guest agent, passt and the transfer folder's virtiofsd killed mid-run, then lord-vm
    itself; Machines reports each and recovers."""
    need_machine(s, c, running=True)
    mdir, mid = c["dir"], c["id"]
    name = json.dumps(NAME)
    s.wait_js("lord.vm.list().find((m) => m.name === %s)?.agentState === 'connected'" % name, 90)
    con = s.console(mid)
    # 1. the guest agent: Machines sees its port close (lord-vm's log, agentState "waiting") and takes it back when the
    # agent is there again (systemd may start it again at once in the guest; if not, the test starts it)
    n0 = s.vmlog(mdir).count("the guest closed qemu-guest-agent's port")
    con.run("systemctl kill -s KILL qemu-guest-agent")
    r.step("kill: the guest agent killed in the guest is seen (its port closed, the agent waited for)", lambda: _until(
        lambda: s.vmlog(mdir).count("the guest closed qemu-guest-agent's port") > n0, 30, 0.5))
    try:
        _until(lambda: "agent: connected" in s.vmlog(mdir).split("the guest closed qemu-guest-agent's port")[-1], 20, 1)
        c["agent_back"] = "by itself"
    except TimeoutError:
        con.run("systemctl start qemu-guest-agent")
        c["agent_back"] = "started again by the test"
    r.step("kill: the agent connects again (%s)" % c["agent_back"], lambda: s.wait_js(
        "lord.vm.list().find((m) => m.name === %s)?.agentState === 'connected'" % name, 90))
    # 2. passt (the shared network): lord-vm attaches a new one and the guest's network comes back
    run = json.load(open(os.path.join(mdir, "run.json")))
    pp = _child(run.get("pid"), "passt")
    if pp:
        t0 = time.time()
        os.kill(pp, 9)
        r.step("kill: passt killed: lord-vm says the network is back", lambda: _wait_log(s, mdir, re.compile(r"the network is back"), 30))
        out, st = con.run("for i in 1 2 3 4 5 6 7 8 9 10; do getent ahosts deb.debian.org >/dev/null && echo NET-OK && break; sleep 1; done", 30)
        r.t("kill: the guest's network works again (%.1f s)" % (time.time() - t0), "NET-OK" in out, out)
    else:
        r.skp("kill: passt", "no passt under lord-vm %s" % run.get("pid"))
    # 3. the transfer folder's virtiofsd: reported; the machine keeps running
    vp = _child(run.get("pid"), "virtiofsd")
    if vp:
        os.kill(vp, 9)
        r.step("kill: virtiofsd killed: lord-vm logs that the folder is gone until the next start", lambda: _wait_log(s, mdir, re.compile(r"virtiofsd exited .*|is gone from the machine until it starts again"), 20))
        r.t("kill: the machine keeps running after virtiofsd", s.vm(NAME).get("state") == "running")
        s.js("wm.focus(%s)" % json.dumps(vm_window(s, NAME)["id"]))
        r.step("kill: the machine's window says the transfer folder stopped (VMN_SHARE_LOST)", lambda: s.wait_text(
            re.compile(r"transfer folder stopped working"), 10, s.rect_of(vm_window(s, NAME))))
        s.shot("12-virtiofsd-killed")
    else:
        r.skp("kill: virtiofsd", "no virtiofsd under lord-vm (transfer folder off?)")
    con.close()
    # 4. lord-vm itself: the app says its supervisor ended, a new start recovers it
    lv = run.get("pid")
    os.kill(lv, 9)
    m = r.step("kill: lord-vm killed: the library says supervisor-lost", lambda: _wait_reason(s, NAME, "supervisor-lost", 30))
    s.shot("13-supervisor-lost")
    s.js("wm.focus('machines')")
    time.sleep(0.5)
    s.click_text(NAME, within=lib_rect(s), n=2)
    r.step("kill: started again, it runs (recovered)", lambda: s.wait_state(NAME, "running", 90))
    r.t("kill: lord-vm's log says what it recovered", re.search(r"recover", s.vmlog(mdir)) is not None)


def _wait_reason(s, name, reason, timeout):
    end = time.time() + timeout
    while time.time() < end:
        m = s.vm(name)
        if m.get("reason") == reason:
            return m
        time.sleep(0.5)
    raise TimeoutError("reason %s, not %s" % (s.vm(name).get("reason"), reason))


def _child(pid, comm):
    """A descendant of pid whose name starts with comm (passt is passt.avx2 on amd64)."""
    if not pid:
        return None
    try:
        out = subprocess.run(["ps", "-e", "-o", "pid=,ppid=,comm="], capture_output=True, text=True).stdout
    except OSError:
        return None
    kids = {}
    for l in out.splitlines():
        p, pp, cm = l.split(None, 2)
        kids.setdefault(int(pp), []).append((int(p), cm.strip()))
    todo = [int(pid)]
    while todo:
        x = todo.pop()
        for k, cm in kids.get(x, []):
            if cm.startswith(comm):
                return k
            todo.append(k)
    return None


def flow_configure(s, r, c):
    """Configure… from the card's menu (the machine off; a sheet over the library): Network → Isolated, Done; again,
    Guest agent → on, Done; again, Network → Shared, Done. Each time machine.json says what was chosen."""
    need_machine(s, c, running=False)
    mj = os.path.join(c["dir"], "machine.json")
    card_menu(s, NAME, "Configure…")
    w = lib_rect(s)
    s.wait_text("Startup and shutdown", 10, w)
    s.shot("14-configure")
    s.click_text("Network", within=w)
    s.click_text("Isolated", within=w)
    s.shot("15-configure-network")
    s.click_text("Done", within=w)
    r.step("configure: Network → Isolated is saved", lambda: _wait_file(mj, '"network": "isolated"', 20))
    # the guest agent: the section list scrolls (it is under Sharing), the pane's first row is its switch
    card_menu(s, NAME, "Configure…")
    w = lib_rect(s)
    side = s.wait_text("Startup and shutdown", 10, w)["rect"]
    s.remote.wheel(side[0] + 40, side[1] + 150, -8)
    time.sleep(0.5)
    sections = [t for t in s.find("Guest agent", w, all=True) if t["rect"][0] < side[0] + side[2]]
    s.click(sections[0]["rect"][0] + 20, sections[0]["rect"][1] + 8)
    time.sleep(0.5)
    rows = [t for t in s.find("Guest agent", w, all=True) if t["rect"][0] > side[0] + side[2] and t["rect"][3] < 22]
    x, y, ww, hh = rows[0]["rect"]
    s.click(x + 22, y + hh + 20)                    # the switch under the row's label
    s.shot("16-configure-agent")
    s.click_text("Done", within=w)
    r.step("configure: Guest agent → on is saved", lambda: _wait_file(mj, '"agent": true', 20))
    card_menu(s, NAME, "Configure…")
    w = lib_rect(s)
    s.click_text("Network", within=w)
    s.click_text("Shared", within=w)
    s.click_text("Sharing", within=w)
    s.click_text("Both ways", within=w)
    s.shot("16-configure-clipboard")
    s.click_text("Done", within=w)
    r.step("configure: Network → Shared and Clipboard → Both ways are saved", lambda: _wait_file(mj, '"network": "shared"', 20) and
           _wait_file(mj, '"clipboard": "both"', 5))


def _wait_file(path, text, timeout):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if text in open(path).read():
                return True
        except OSError:
            pass
        time.sleep(0.4)
    raise TimeoutError("%s doesn't say %s" % (path, text))


def _snaps(s, c):
    return [x["name"] for x in json.loads(s.lordvm("--snapshot", c["dir"], "list").stdout or "{}").get("snapshots", [])]


def flow_snapshots(s, r, c):
    """Snapshots… (the machine off): Take Snapshot → Take; select it, Go Back to This (a snapshot of now first, as offered);
    Delete it. lord-vm's list agrees at each step."""
    need_machine(s, c, running=False)
    before = _snaps(s, c)
    card_menu(s, NAME, "Snapshots…")
    sw = s.window("machines", lambda w: w["title"].endswith("snapshots"), timeout=15)
    R = s.rect_of(sw)
    s.click_text("Take Snapshot", within=R)
    name = s.wait_text(re.compile(r"^Snapshot \d+$"), 10, R)["text"]      # the suggested name
    s.click_text("Take", within=R, nth=-1)
    r.step("snapshots: Take makes one", lambda: s.wait_text("Snapshot taken.", 30, R))
    r.t("snapshots: lord-vm lists %r" % name, name in _snaps(s, c), _snaps(s, c))
    s.shot("17-snapshot-taken")
    s.click_text(name, within=R)
    s.click_text("Go Back to This", within=R)
    s.wait_text(re.compile(r"Go back to “%s”\?" % re.escape(name)), 10, R)
    s.click_text("Go Back to This", within=R, nth=-1)
    r.step("snapshots: Go Back to This works", lambda: s.wait_text("The machine went back to the snapshot.", 30, R))
    r.t("snapshots: the state before it was kept as a snapshot first", "Before going back to %s" % name in _snaps(s, c), _snaps(s, c))
    s.click_text(name, within=R)
    s.click_text("Delete", within=R)
    s.wait_text("Delete “%s”?" % name, 10, R)
    s.click_text("Delete", within=R, nth=-1)
    r.step("snapshots: Delete removes it", lambda: s.wait_text("Snapshot deleted.", 30, R))
    left = _snaps(s, c)
    r.t("snapshots: lord-vm no longer lists it", name not in left and len(left) == len(before) + 1, left)
    s.shot("18-snapshot-deleted")
    s.js("wm.close(%s)" % json.dumps(sw["id"]))


def flow_clone(s, r, c):
    """Clone or Duplicate… (the machine off): Clone → a second machine in the library, with its own id and MAC address and
    the name inside the machine the sheet suggested."""
    need_machine(s, c, running=False)
    card_menu(s, NAME, "Clone or Duplicate…")
    w = lib_rect(s)
    s.wait_text("Copy %s" % NAME, 10, w)
    how = [t["text"] for t in s.find(re.compile(r"^This disk is on|^A full copy"), w, all=True)]
    c["clone_how"] = how[-1] if how else ""
    s.shot("19-clone")
    s.click_text("Clone", within=w, nth=-1)
    m = r.step("clone: the new machine is in the library", lambda: s.vm(CLONE, timeout=300))
    if not m:
        return
    c["clone_dir"] = m["dir"]
    mj = json.load(open(os.path.join(m["dir"], "machine.json")))
    orig = json.load(open(os.path.join(c["dir"], "machine.json")))
    r.t("clone: its own id and MAC address", mj.get("id") != orig.get("id") and mj.get("mac") != orig.get("mac"), (mj.get("id"), mj.get("mac")))
    r.step("clone: its card is shown", lambda: s.wait_text(CLONE, 10, lib_rect(s)))
    s.shot("20-cloned")


def flow_storage(s, r, c):
    """Storage figures (12): what Machines says deleting the clone gives back (lord.vm.storage()'s `frees`, which the Storage
    window shows as "Frees if deleted"), against the change in free space once it is in the Trash and the Trash is emptied, on
    this run's file system (MH_SCRATCH; ext4, btrfs and XFS each need their own run)."""
    need_machine(s, c)
    if "clone_dir" not in c:
        skip("no clone")
    fs = subprocess.run(["stat", "-f", "-c", "%T", c["clone_dir"]], capture_output=True, text=True).stdout.strip()
    st = s.js("return lord.vm.storage()")
    clone = next((m for m in st.get("machines", []) if m.get("name") == CLONE), None)
    orig = next((m for m in st.get("machines", []) if m.get("name") == NAME), None)
    if not clone:
        raise RuntimeError("lord.vm.storage() doesn't list the clone: %s" % [m.get("name") for m in st.get("machines", [])])
    s.shot("21-storage")
    vfs = os.statvfs(c["clone_dir"])
    free0 = vfs.f_bavail * vfs.f_frsize
    card_menu(s, CLONE, "Remove…")
    s.click_text("Move to Trash")
    r.step("storage: the clone goes to the Trash", lambda: _gone(s, CLONE, 30))
    s.js("for (const t of sdk.fs.trashList() || []) sdk.fs.removeFromTrash(t.id)")
    time.sleep(2)
    vfs = os.statvfs(c["dir"])
    freed = vfs.f_bavail * vfs.f_frsize - free0
    said = clone.get("frees") or 0
    c["storage"] = {"fs": fs, "clone_bytes": clone.get("total"), "said_frees": said, "freed": freed, "clone_base": clone.get("base") or "",
                    "orig_bytes": orig and orig.get("total"), "how": c.get("clone_how", ""), "shared": clone.get("shared"),
                    "approx": bool(clone.get("approx"))}
    # compressed extents (btrfs with compression): the figures are "about", and the window says so (6.16)
    if os.environ.get("MH_EXPECT_APPROX") == "1":
        r.t("storage: compressed extents make the figures \"about\"", bool(clone.get("approx")) or bool(orig and orig.get("approx")), c["storage"])
    log("storage: %s; Machines said deleting the clone frees %.1f MB, the free space grew %.1f MB" % (fs, said / 1e6, freed / 1e6))
    tol = max(32 << 20, said * 0.05)
    r.t("storage: what Machines said the clone frees (%.1f MB) is what deleting it gave back (%.1f MB) on %s" % (said / 1e6, freed / 1e6, fs),
        abs(freed - said) <= tol, c["storage"])


def _gone(s, name, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if not any(m.get("name") == name for m in s.vms()):
            return True
        time.sleep(0.5)
    raise TimeoutError("%s is still in the library" % name)


def flow_group(s, r, c):
    """A machine group's card (19.8): a group made by lord-vm's test build from two empty disks, shown in the library."""
    if platform.machine() != "x86_64":
        skip("needs an x86-64 host (Whonix is amd64 only; lord-vm refuses it elsewhere)")
    tp = os.path.join(BUILD, "lord-vm-testpaths")
    if not os.access(tp, os.X_OK):
        subprocess.run(["make", "-s", "build/lord-vm-testpaths"], cwd=os.path.dirname(BUILD), capture_output=True)
    if not os.access(tp, os.X_OK):
        skip("needs build/lord-vm-testpaths")
    G = os.path.join(s.library, "Whonix.lordgroup")
    for d in ("gw", "ws"):
        subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", os.path.join(s.D, d + ".qcow2"), "1G"], check=True)
    cat = os.path.realpath(os.path.join(os.path.dirname(BUILD), "..", "assets", "machines", "systems.json"))
    s.ns([tp, "--group-create", G, "--catalog-id", "whonix", "--dev-gateway", os.path.join(s.D, "gw.qcow2"),
          "--dev-workstation", os.path.join(s.D, "ws.qcow2")], env={"LORDVM_TEST_CATALOG": cat})
    s.js("wm.focus('machines')")
    w = lib_rect(s)
    r.step("group: the library shows the group's card", lambda: s.wait_text(re.compile(r"Whonix"), 20, w))
    r.step("group: with its two machines", lambda: s.wait_text(re.compile(r"Gateway|Workstation|2 machines"), 10, w))
    img = s.shot("19-group-card")
    r.t("group: the card is drawn", img.varied(w, 40))


WNAME = "MH Windows"


def flow_windows(s, r, c):
    """The virtio-win install test (docs/VIRTUALIZATION.md 4.3.1, 12), not one of the default flows (about 40 minutes, 9 GB
    of downloads the first time): Microsoft's Windows 11 Enterprise Evaluation and the pinned virtio-win, both from their
    makers' servers with the SHA-256 the catalog pins (images.py --windows), given to lord.vm.create as the assistant's
    evaluation card gives them (UEFI with Secure Boot, a TPM, Hyper-V, the windows devices, Install without questions);
    started from the library card; done when lord-vm says Windows is installed and the LordOS tools for Windows say hello
    from the installed system. Its picture is kept."""
    if not kvm_ok():
        skip("needs /dev/kvm")
    iso, drv = os.path.join(s.home, "Images", "win11-eval-en-us.iso"), os.path.join(s.home, "Images", "virtio-win.iso")
    if not (os.path.exists(iso) and os.path.exists(drv)):
        skip("the Windows evaluation and virtio-win aren't downloaded (python3 -I tools/vm/e2e/machines/images.py --windows)")
    req = {"name": WNAME, "arch": "amd64", "vcpus": 4, "memoryMb": 8192, "diskGb": 64, "iso": iso, "windows": {"drivers": drv},
           "unattended": {"kind": "windows", "guided": False, "user": "tester", "password": "Vm-guest-0001", "fullname": "",
                          "hostname": "mh-win", "edition": "", "key": "", "uilang": "en-US", "inputloc": "0409:00000409",
                          "userloc": "en-US", "wtz": "UTC"}}
    job = s.js("return lord.vm.create(%s)" % json.dumps(req))
    r.t("windows: lord.vm.create takes the evaluation, the drivers and the answers", bool(job), job)
    done = s.wait_js("(() => { const j = lord.vm.job(%d); return j && j.done ? j : null; })()" % job, 600)
    r.t("windows: the machine is made", done.get("ok"), done.get("out", "")[-600:])
    m = s.vm(WNAME, timeout=30)
    c["win_dir"] = m["dir"]
    mj = json.load(open(os.path.join(m["dir"], "machine.json")))
    r.t("windows: Secure Boot, a TPM, Hyper-V and the windows devices", mj.get("secure_boot") and mj.get("tpm") and mj.get("hyperv") and
        mj.get("devices") == "windows", {k: mj.get(k) for k in ("secure_boot", "tpm", "hyperv", "devices", "firmware")})
    t0 = time.time()
    s.click_text(WNAME, within=lib_rect(s), n=2)
    r.step("windows: it starts from its card", lambda: s.wait_state(WNAME, "running", 120))
    allow_shortcuts(s, 8)
    shots = 0

    def installed():
        nonlocal shots
        lg = s.vmlog(m["dir"])
        if int(time.time() - t0) // 300 > shots:            # a picture every five minutes of the install
            shots += 1
            try:
                s.shot("windows-install-%02d" % shots)
            except Exception:                                  # noqa: BLE001
                pass
        if s.vm(WNAME).get("state") in ("crashed",):
            raise RuntimeError("the machine stopped: %s" % lg.splitlines()[-3:])
        return "unattended: Windows is installed" in lg
    ok = r.step("windows: Setup installs it without questions (lord-vm: Windows is installed)", lambda: _until(installed, 5400, 10))
    c["win_install_min"] = round((time.time() - t0) / 60, 1)
    if not ok:
        return
    hello = re.compile(r"agent: lordos-guest (w\d+) said hello")
    r.step("windows: the LordOS tools for Windows say hello from the installed system",
           lambda: _until(lambda: hello.search(s.vmlog(m["dir"])), 1200, 10))
    hv = hello.search(s.vmlog(m["dir"]))
    c["win_tools"] = hv.group(1) if hv else None
    allow_shortcuts(s)
    time.sleep(20)
    img = s.shot("windows-desktop")
    vr = s.rect_of(vm_window(s, WNAME))
    r.t("windows: its desktop is drawn in the machine's window", img.varied([vr[0] + 8, vr[1] + 100, vr[2] - 16, vr[3] - 110], 40))
    c["win_total_min"] = round((time.time() - t0) / 60, 1)
    s.js("wm.focus(%s)" % json.dumps(vm_window(s, WNAME)["id"]))
    menu(s, "Actions", "Shut Down")
    r.step("windows: Shut Down (the guest agent) turns it off", lambda: s.wait_state(WNAME, "stopped", 300))


def _until(fn, timeout, every=1):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(every)
    raise TimeoutError("not after %s s" % timeout)


# ---------------------------------------------------------------- what a person sees of a running machine (sweep 3)
# run.py premade s3prep start agent s3window s3guest s3links s3send s3shell s3agentlost s3monitor: the machine window's
# bar and menus, the guest's links and notifications, Send to LordOS and the file copies panel, the agent's shell pane and
# its Terminal tab, the agent's red light, and the machine's row in Monitor, each on the headless session's screen with
# the real lord-vm and a real KVM guest. The guest's tools are this build's lordos-guest (10), put in through the transfer
# folder and run as the guest's person (tester) as the desktop session's user unit would run it.

_EN = None


def T(key, **v):
    """Machines' English text for a key (assets/locales/en/machines.json), {name} filled in."""
    global _EN
    if _EN is None:
        _EN = json.load(open(os.path.join(os.path.dirname(BUILD), "..", "assets", "locales", "en", "machines.json")))
    s = _EN[key]
    for k, x in v.items():
        s = s.replace("{%s}" % k, str(x))
    return s


def _vm_front(s):
    allow_shortcuts(s)
    vw = vm_window(s, NAME)
    s.js("wm.focus(%s)" % json.dumps(vw["id"]))
    time.sleep(0.4)
    return s.rect_of(vm_window(s, NAME))


def _guest(s, c, cmd, timeout=60):
    con = s.console(c["id"])
    try:
        return con.run(cmd, timeout=timeout)
    finally:
        con.close()


def flow_s3prep(s, r, c):
    """The premade machine with the guest agent, the guest's links and notifications on (off by default), and the transfer
    folder; changed with the machine off, as Configure does."""
    if "dir" not in c:
        skip("no machine (premade first)")
    s.lordvm("--set", c["dir"], "--agent", "on", "--guest-links", "on", "--guest-notify", "on", "--transfer", "on", "--clipboard", "both")
    mj = json.load(open(os.path.join(c["dir"], "machine.json")))
    r.t("s3prep: agent, links, notifications and the transfer folder on", mj.get("agent") and mj.get("guest_links") and mj.get("guest_notify") and mj.get("transfer"),
        {k: mj.get(k) for k in ("agent", "guest_links", "guest_notify", "transfer")})


def flow_s3window(s, r, c):
    """The machine window's bar and its menus in a running window (5.3): Actions, Devices, View (Scale, Picture in
    Picture) and Info in the menu bar, the toolbar's buttons and device dots, each menu open on screen."""
    need_machine(s, c, running=True)
    vr = _vm_front(s)
    img = s.shot("s3-01-window")
    bar = s.texts([0, 0, s.W, 28])
    r.t("s3window: the menu bar has Actions, Devices, View and Info", all(T(k) in bar for k in ("menu.actions", "menu.devices", "lib.view", "menu.info")), bar)
    tb = s.find_image("-symbolic.svg", [vr[0], vr[1], vr[2], 90], all=True) or []
    srcs = sorted({os.path.basename(x.get("src", "")) for x in tb})
    r.t("s3window: the toolbar's buttons are drawn (%d symbols)" % len(tb), len(tb) >= 4 and any("terminal" in x for x in srcs), srcs)
    r.t("s3window: the guest's picture is drawn under the bar", img.varied([vr[0] + 8, vr[1] + 100, vr[2] - 16, vr[3] - 140], 2))
    want = {"menu.actions": ("cmd.pause", "tb.transferOpen", "cmd.logs"), "menu.devices": (), "lib.view": ("view.scale", "view.pip"), "menu.info": ()}
    for i, (m, keys) in enumerate(want.items()):
        s.click_text(T(m), within=[0, 0, s.W, 28])
        time.sleep(0.6)
        shot = s.shot("s3-02-menu-%d-%s" % (i, m.split(".")[1]))
        got = s.texts()
        r.t("s3window: %s opens%s" % (T(m), (" with " + ", ".join(T(k) for k in keys)) if keys else ""),
            all(T(k) in got for k in keys) and shot.varied([0, 28, s.W, 300], 2), [x for x in got if len(x) < 40][:60])
        s.press("esc")
        time.sleep(0.4)
    r.t("s3window: no JS error so far", not s.errors(), s.errors()[:5])


def flow_s3guest(s, r, c):
    """This build's lordos-guest (10) into the guest through the transfer folder (/mnt/LordOS, mounted by the agent), run
    as the guest's person with the port theirs (what the helper's udev rule and user unit do in a desktop session)."""
    need_machine(s, c, running=True)
    xf = os.path.join(s.library, "Transfer", NAME)
    r.step("s3guest: the transfer folder is there", lambda: _until(lambda: os.path.isdir(xf), 60))
    shutil.copyfile(os.path.join(BUILD, "lordos-guest"), os.path.join(xf, "lordos-guest"))
    con = s.console(c["id"])
    out, st = con.run("for i in $(seq 30); do mountpoint -q /mnt/LordOS && [ -s /mnt/LordOS/lordos-guest ] && break; sleep 1; done; "
                      "install -m755 /mnt/LordOS/lordos-guest /usr/local/bin/lordos-guest && /usr/local/bin/lordos-guest --version")
    r.t("s3guest: lordos-guest 10 in the guest (%s)" % out.strip()[:60], "lordos-guest 10" in out, out)
    con.run("chmod 666 /dev/virtio-ports/org.lordos.agent; loginctl enable-linger tester; for i in $(seq 20); do [ -d /run/user/1000 ] && break; sleep 0.5; done; "
            "systemctl stop mh-lg 2>/dev/null; pkill -x lordos-guest; sleep 1; "
            "systemd-run --unit mh-lg --uid=1000 --gid=1000 -E XDG_RUNTIME_DIR=/run/user/1000 -E HOME=/home/tester /usr/local/bin/lordos-guest")
    con.close()
    r.step("s3guest: it says hello to lord-vm as 10", lambda: _wait_log(s, c["dir"], re.compile(r"agent: lordos-guest 10 said hello"), 40))


def _as_tester(cmd):
    return "runuser -u tester -- env XDG_RUNTIME_DIR=/run/user/1000 HOME=/home/tester " + cmd


def flow_s3links(s, r, c):
    """A link the guest sends (6.12) shows in the window's bar with Open and Ignore and is opened only from there; a
    notification of the guest's shows as from the machine."""
    need_machine(s, c, running=True)
    vr = _vm_front(s)
    url = "https://lordos.org/mh-%s" % os.urandom(3).hex()
    out, st = _guest(s, c, _as_tester("/usr/local/bin/lordos-guest --open-on-host %s; echo rc=$?" % url))
    r.step("s3links: the bar offers the link (Open, Ignore)", lambda: s.wait_text(T("link.hint"), 15, vr))
    shot = s.shot("s3-03-link-bar")
    got = s.texts(vr)
    r.t("s3links: the bar names the link and has Open and Ignore", any(url in x for x in got) and T("link.open") in got and T("link.ignore") in got, got[:40])
    s.click_text(T("link.ignore"), within=vr)
    r.step("s3links: Ignore takes the bar away (nothing opened)", lambda: s.gone_text(T("link.hint"), 10, vr))
    r.t("s3links: no browser started", not any(w["id"] not in ("machines",) and "browser" in w["id"] for w in s.windows()), [w["id"] for w in s.windows()])
    title = "MH note %s" % os.urandom(2).hex()
    _guest(s, c, _as_tester("/usr/local/bin/lordos-guest --notify-host '%s' 'from inside the machine'; echo rc=$?" % title))
    t = r.step("s3links: the guest's notification is shown", lambda: s.wait_text(title, 15))
    s.shot("s3-04-guest-notification")
    near = s.texts([0, 0, s.W, 220]) if t else []
    r.t("s3links: as from the machine (%s)" % T("note.from", name=NAME), any(NAME in x for x in near), near[:20])


def flow_s3send(s, r, c):
    """Send to LordOS from the guest (6.21): the window's file copies panel shows it coming and done, the file lands in the
    machine's quarantine folder (<library>/Received/<name>) whole, marked, 0600."""
    need_machine(s, c, running=True)
    vr = _vm_front(s)
    out, st = _guest(s, c, "head -c 24M /dev/urandom > /home/tester/mh-send.bin; chown tester: /home/tester/mh-send.bin; sha256sum /home/tester/mh-send.bin; "
                     + _as_tester("/usr/local/bin/lordos-guest --send-to-host /home/tester/mh-send.bin") + "; echo rc=$?")
    m = re.search(r"([0-9a-f]{64})", out)
    want = m.group(1) if m else None
    r.t("s3send: the guest's tools take the file (rc=0)", "rc=0" in out and want, out[-300:])
    r.step("s3send: the file copies panel shows it", lambda: s.wait_text(re.compile(r"mh-send\.bin"), 20, vr))
    s.shot("s3-05-send-panel")
    dst = os.path.join(s.library, "Received", NAME, "mh-send.bin")
    r.step("s3send: it arrives in the quarantine folder", lambda: _until(lambda: os.path.exists(dst) and os.path.getsize(dst) == 24 << 20, 60))
    time.sleep(1)
    s.shot("s3-06-send-done")
    import hashlib
    h = hashlib.sha256(open(dst, "rb").read()).hexdigest() if os.path.exists(dst) else None
    st_ = os.stat(dst) if os.path.exists(dst) else None
    r.t("s3send: whole (SHA-256 as in the guest), 0600, in a 0700 folder", h == want and st_ and (st_.st_mode & 0o777) == 0o600 and
        (os.stat(os.path.dirname(dst)).st_mode & 0o777) == 0o700, [h, want, oct(st_.st_mode) if st_ else None])
    try:
        mark = os.getxattr(dst, "user.lordos.quarantine")
    except OSError:
        mark = None
    r.t("s3send: marked as from a machine (user.lordos.quarantine)", mark == b"1", mark)
    got = s.texts(vr)
    r.t("s3send: the panel says received", any(T("xfer.received") in x for x in got) or any("arrived" in x for x in s.texts()), got[:40])


def flow_s3shell(s, r, c):
    """The agent's shell (6.18): the toolbar's Shell opens the pane with the guest person's own shell, typed into from
    here; Open in Terminal moves it into a Terminal tab marked as the machine's."""
    need_machine(s, c, running=True)
    vr = _vm_front(s)
    s.click_icon("terminal", within=vr)
    r.step("s3shell: the pane opens (GUEST)", lambda: s.wait_text(T("shell.badge"), 10, vr))
    time.sleep(3)
    word = "mh%s" % os.urandom(2).hex()
    s.type("echo %s-$((6*7)) > /tmp/%s; id -un\n" % (word, word))
    time.sleep(2)
    s.shot("s3-07-shell-pane")
    out, st = _guest(s, c, "cat /tmp/%s; stat -c %%U /tmp/%s" % (word, word))
    r.t("s3shell: what was typed ran in the guest as the person (tester), not root", "%s-42" % word in out and "tester" in out, out)
    if s.find(T("shell.toTerminal"), vr):
        s.click_text(T("shell.toTerminal"), within=vr)
        r.step("s3shell: Open in Terminal moves it (the pane says so)", lambda: s.wait_text(T("shell.inTerminal"), 15))
        time.sleep(1.5)
        s.shot("s3-08-shell-terminal")
        r.t("s3shell: a Terminal window has it", any(w["id"].startswith("terminal") or w.get("app") == "terminal" for w in s.windows()), [w["id"] for w in s.windows()])
    else:
        r.bad("s3shell: Open in Terminal", "no button (texts: %s)" % s.texts(vr)[:30])


def flow_s3agentlost(s, r, c):
    """The agent's light (6.17): red when the agent stops answering while the guest runs; the popover says so; green again."""
    need_machine(s, c, running=True)
    _vm_front(s)
    for w in s.windows():
        if w["id"].startswith("terminal"):
            s.js("wm.close(%s)" % json.dumps(w["id"]))
    vr = _vm_front(s)
    # the shell pane falls back to the serial console when the agent's shell goes, and holds the console the checks use:
    # closed first (its ×)
    if s.find(T("shell.badge"), vr):
        s.click_text("×", within=[vr[0], vr[1] + 100, vr[2], vr[3] - 100], nth=-1)
        time.sleep(0.8)
    s.shot("s3-09-agent-ok")
    _guest(s, c, "systemctl stop qemu-guest-agent; echo done")
    r.step("s3agentlost: lord.vm.list says the agent no longer answers", lambda: s.wait_js(
        "(() => { const a = lord.vm.list().find((m) => m.name === %s)?.agentState; return a && a !== 'connected'; })()" % json.dumps(NAME), 90))
    time.sleep(2)
    s.click_icon("agent", within=vr)
    time.sleep(1)
    s.shot("s3-10-agent-lost")
    got = s.texts(vr)
    r.t("s3agentlost: the popover says it stopped answering (%s), not that it was never installed" % T("agent.lost"), T("agent.lost") in got and T("agent.lostHint") in got
        and T("agent.installHint") not in got, got[:30])
    s.click(vr[0] + vr[2] - 60, vr[1] + vr[3] - 40)
    _guest(s, c, "systemctl start qemu-guest-agent; echo done")
    r.step("s3agentlost: connected again", lambda: s.wait_js("lord.vm.list().find((m) => m.name === %s)?.agentState === 'connected'" % json.dumps(NAME), 60))


def flow_s3monitor(s, r, c):
    """Monitor → Processes with the machine running: its row (named after the machine when its QEMU runs in the machine's
    scope; the harness's session has none, so wherever its processes are) and Show Machine."""
    need_machine(s, c, running=True)
    s.js("launchApp('monitor')")
    mw = s.window("monitor", timeout=15)
    s.js("wm.setFrame(%s, { x: 30, y: 40, w: 1180, h: 700 })" % json.dumps(mw["id"]))
    time.sleep(1.5)
    mr = s.rect_of(s.window("monitor"))
    if s.find("Processes", mr):
        s.click_text("Processes", within=mr)
    time.sleep(1)
    if s.find("Grouped", mr):
        s.click_text("Grouped", within=mr)
    time.sleep(1)
    s.click_text("Search", within=mr)
    s.type("qemu")
    time.sleep(2.5)
    s.shot("s3-11-monitor")
    got = s.texts(mr)
    c["monitor_texts"] = [x for x in got if "qemu" in x.lower() or NAME in x or "Disk" in x][:20]
    r.t("s3monitor: Monitor lists the machine's QEMU (searched)", any("qemu" in x.lower() or NAME in x for x in got), got[:60])
    s.js("wm.close(%s)" % json.dumps(s.window("monitor")["id"]))


EXTRA = ["premade", "windows", "s3prep", "s3window", "s3guest", "s3links", "s3send", "s3shell", "s3agentlost", "s3monitor"]
FLOWS = ["library", "assistant", "lifecycle", "configure", "start", "agent", "clipboard", "filedrop", "killcopy", "kills", "snapshots", "clone", "storage", "group"]
