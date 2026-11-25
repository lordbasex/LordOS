# win-agent.py — the LordOS tools for Windows in a real Windows 11 guest, through lordvm-dev (which plays lord-server's
# part; docs/VIRTUALIZATION.md 6.12, 6.18, 6.19, 6.21, 7.1): a machine the `windows` flow installed (Windows 11
# Enterprise Evaluation, virtio-win), started with this build's lordos-guest-win (LORDVM_WINTOOLS, lord-vm --dev-qmp),
# its tools updated through the agent (Install LordOS Tools), then:
#   - the agent's shell: PowerShell as the signed-in person, not elevated (6.18);
#   - a link and a notification from the guest's tools (--open-on-host, --notify-host; 6.12);
#   - Send to LordOS (--send-to-host): the file in the machine's quarantine folder, whole, marked, 0600 (6.21);
#   - the System and Application event logs in log/journal.log through qemu-guest-agent (6.19);
#   - apps mode: the guest lists its windows and its programs (7.2).
# Run it in the harness's namespaces (your home and session out of reach), the machine off:
#   sh tools/vm/e2e/machines/ns-run.sh python3 -I tools/vm/e2e/machines/win-agent.py MACHINE_DIR
# Exit 0 passed, 1 a failure.
import hashlib, json, os, re, select, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.normpath(os.path.join(HERE, "..", "..", "..", "..", "server"))
BUILD = os.path.join(SERVER, "build")
D = os.path.abspath(sys.argv[1])
NAME = os.path.basename(D)[:-len(".lordvm")]
p = f = 0


def t(name, ok, extra=""):
    global p, f
    if ok:
        p += 1
        print("PASS  " + name, flush=True)
    else:
        f += 1
        print("FAIL  %s%s" % (name, (": " + str(extra)[:600]) if extra != "" else ""), flush=True)


def _log_ino():
    try:
        return os.stat(os.path.join(D, "log", "lord-vm.log")).st_ino
    except OSError:
        return None


OLD_LOG = _log_ino()        # lord-vm starts a new lord-vm.log at each start (the last one becomes lord-vm.1.log)


env = dict(os.environ, LORDVM_WINTOOLS=os.path.join(BUILD, "lordos-guest-win.exe"))
dev = subprocess.Popen([os.path.join(BUILD, "lordvm-dev"), "run", D, "--dev-qmp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                       stderr=open(os.path.join(os.environ.get("MH_NS_DIR", "/tmp"), "dev.err"), "w"), env=env, text=True, bufsize=1)
events = []


def pump(sec):
    end = time.time() + sec
    while time.time() < end:
        r, _, _ = select.select([dev.stdout], [], [], 0.2)
        if r:
            line = dev.stdout.readline()
            if not line:
                return
            try:
                events.append(json.loads(line))
            except ValueError:
                events.append({"raw": line})


def say(cmd):
    dev.stdin.write(cmd + "\n")
    dev.stdin.flush()


def vmlog():
    try:
        with open(os.path.join(D, "log", "lord-vm.log"), "rb") as fh:
            if OLD_LOG is not None and os.fstat(fh.fileno()).st_ino == OLD_LOG:
                return ""                       # still the last run's
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def until(fn, sec, every=1.0):
    end = time.time() + sec
    while time.time() < end:
        v = fn()
        if v:
            return v
        pump(every)
    return None


shell_out = []


ESC = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07]*\x07|\x1b[()][0-9A-Za-z]")


def shell_text():
    """what the shell wrote, without the pseudo console's escapes"""
    return ESC.sub("", "".join(e.get("text", "") for e in events if e.get("ev") == "shell"))


def ps(cmd, mark, sec=60):
    """a PowerShell line typed into the agent's shell; waits until mark is in its output"""
    n0 = len(shell_text())
    say("shell-type " + cmd + "\r")              # Enter in a pseudo console is a carriage return
    ok = until(lambda: mark in shell_text()[n0:], sec, 0.5)
    return shell_text()[n0:] if ok else None


try:
    t0 = time.time()
    hello = re.compile(r"agent: lordos-guest (w\d+) said hello")
    # the tools start in the person's session: the install's automatic sign-in was for its first starts only, so the
    # person signs in at the lock screen (the `windows` flow's account), typed as a person would
    until(lambda: "agent: connected" in vmlog(), 300, 2)
    v = until(lambda: hello.search(vmlog()), 60, 2)
    if not v:
        say("key 28")
        pump(4)
        say("type %s\\n" % os.environ.get("MH_WIN_PASSWORD", "Vm-guest-0001"))
        v = until(lambda: hello.search(vmlog()), 300, 2)
        print("      signed in at the lock screen: %s" % ("the tools said hello" if v else "no hello"))
    t("the installed tools say hello (%s, %.0f s after the start)" % (v.group(1) if v else "-", time.time() - t0), bool(v), vmlog()[-800:])
    first = v.group(1) if v else ""
    if first != "w13":
        say("agent install")
        w13 = until(lambda: re.search(r"agent: lordos-guest w13 said hello", vmlog()), 300, 2)
        t("Install LordOS Tools puts this build's w13 in and it says hello", bool(w13), vmlog()[-1200:])
    else:
        t("the guest already has w13", True)
    caps = re.findall(r"agent: lordos-guest w13 said hello[^\n]*", vmlog())
    print("      " + (caps[-1] if caps else ""))

    # the agent's shell: PowerShell as the person, never elevated (6.18)
    say("shell 120 30")
    t("the agent's shell starts (PowerShell)", until(lambda: "PS " in shell_text(), 60, 0.5), shell_text()[-400:])
    out = ps("whoami; [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole('Administrators'); 'MH-'+(6*7)", "MH-42")
    print("      shell: " + re.sub(r"\x1b\[[0-9;?]*[A-Za-z]|\\u001b\[[0-9;?]*[A-Za-z]", "", (out or "")).replace("\\r\\n", " | ")[-300:])
    t("it runs as the signed-in person (tester)", out is not None and re.search(r"\\tester", out, re.I), out)
    t("not elevated (Administrators: False)", out is not None and "False" in out, out)

    # a link and a notification from the guest's tools (6.12): lord-vm passes them on (machine.json allows both)
    exe = '& "$env:ProgramFiles\\LordOS\\lordos-guest-win.exe"'
    url = "https://lordos.org/mh-win-%s" % os.urandom(3).hex()
    ps("%s --open-on-host %s; 'MH-'+'L'" % (exe, url), "MH-L")
    t("a link from the Windows guest reaches this computer's side", until(lambda: any(e.get("ev") == "note" and e.get("kind") == "link" and url in e.get("text", "") for e in events), 30, 0.5),
      [e for e in events if e.get("ev") == "note"])
    title = "MH win note %s" % os.urandom(2).hex()
    ps("%s --notify-host '%s' 'from Windows'; 'MH-'+'N'" % (exe, title), "MH-N")
    t("a notification from the Windows guest", until(lambda: any(e.get("ev") == "note" and e.get("kind") == "notify" and title in e.get("title", "") for e in events), 30, 0.5),
      [e for e in events if e.get("ev") == "note"])

    # Send to LordOS (6.21)
    out = ps("$b=New-Object byte[] (6MB); (New-Object Random 7).NextBytes($b); [IO.File]::WriteAllBytes(\"$env:TEMP\\mh-win.bin\",$b); "
             "(Get-FileHash \"$env:TEMP\\mh-win.bin\").Hash; %s --send-to-host \"$env:TEMP\\mh-win.bin\"; 'MH-'+'S'" % exe, "MH-S", 120)
    want = (re.search(r"([0-9A-F]{64})", out or "") or [None, None])[1]
    got = until(lambda: next((e for e in events if e.get("ev") == "recv" and e.get("state") == "done"), None), 120, 0.5)
    lib = os.path.dirname(D)
    dst = os.path.join(lib, "Received", NAME, "mh-win.bin")
    h = hashlib.sha256(open(dst, "rb").read()).hexdigest().upper() if os.path.exists(dst) else None
    t("Send to LordOS from Windows: the file arrives whole (SHA-256 as in the guest)", got and h and want and h == want, [got, h, want])
    if os.path.exists(dst):
        st = os.stat(dst)
        try:
            mark = os.getxattr(dst, "user.lordos.quarantine")
        except OSError:
            mark = None
        t("in the machine's quarantine folder, 0600 and marked", (st.st_mode & 0o777) == 0o600 and mark == b"1", [oct(st.st_mode), mark])
    say("shell-close")

    # the transfer folder in Windows (6.7, 6.7.1): WinFsp from the image store (the pinned installer vm-image-get kept),
    # checked and installed by lord-vm through the agent, the VirtIO-FS service, a drive labelled LordOS both ways
    store = "/var/lib/lordos/machines/images"
    wf = next((j[:-5] for j in sorted(os.listdir(store)) if j.endswith(".json") and '"winfsp"' in open(os.path.join(store, j)).read()), None) if os.path.isdir(store) else None
    if wf and os.path.exists(os.path.join(store, wf + ".iso")):
        say("transfer %s %s" % (wf, os.path.join(store, wf + ".iso")))
        ok = until(lambda: re.search(r"the VirtIO-FS service started|the guest's VirtIO-FS service serves the folder", vmlog()), 300, 2)
        t("WinFsp installed and the VirtIO-FS service serves the transfer folder", bool(ok), vmlog()[-800:])
        xf = os.path.join(os.path.dirname(D), "Transfer", NAME)
        word = "mh-host-%s" % os.urandom(3).hex()
        if os.path.isdir(xf):
            open(os.path.join(xf, "from-host.txt"), "w").write(word)
        n0 = len(shell_text())
        say("shell 120 30")
        until(lambda: "PS " in shell_text()[n0:], 60, 0.5)
        pump(3)
        # the folder's drive as the person sees it: the file system drive that holds what this computer put there
        out = ps("$d=(Get-PSDrive -PSProvider FileSystem|?{Test-Path -LiteralPath ($_.Root+'from-host.txt')}|Select-Object -First 1).Root; 'drive='+$d; "
                 "Get-Content -LiteralPath ($d+'from-host.txt'); Set-Content -LiteralPath ($d+'from-guest.txt') 'mh-guest'; 'MH-'+'X'", "MH-X", 60)
        print("      transfer in Windows: %s" % " ".join((out or "").split())[-200:])
        t("the drive holds what this computer put there", out is not None and word in out, out)
        g = open(os.path.join(xf, "from-guest.txt"), errors="replace").read().strip() if os.path.exists(os.path.join(xf, "from-guest.txt")) else None
        t("what the guest wrote there is here at once", g == "mh-guest", g)
        say("shell-close")
    else:
        print("      skipped: no WinFsp installer in the image store")

    # the event logs (6.19): lord-vm reads the System and Application logs through qemu-guest-agent every 10 s
    jl = os.path.join(D, "log", "journal.log")
    ev = until(lambda: os.path.exists(jl) and re.search(r"(System|Application)", open(jl, errors="replace").read()), 120, 2)
    txt = open(jl, errors="replace").read() if os.path.exists(jl) else ""
    lines = [l for l in txt.splitlines() if l.strip()]
    print("      journal.log: %d lines; e.g. %s" % (len(lines), " || ".join(lines[-2:])[:300]))
    t("a Windows guest's event logs are read into log/journal.log", bool(ev) and len(lines) > 5, txt[-500:])

    # apps mode (7.2): the guest's windows and its programs
    say("apps on")
    w = until(lambda: next((e for e in events if e.get("ev") == "windows"), None), 60, 0.5)
    say("apps-list")
    a = until(lambda: next((e for e in events if e.get("ev") == "apps"), None), 60, 0.5)
    t("apps mode: the guest lists its windows", bool(w), w)
    t("apps mode: and its programs (%s)" % (a or {}).get("count"), a and a.get("count", 0) > 5, a)
    say("apps off")
    pump(3)
finally:
    say("shutdown")
    until(lambda: "state stopped" in vmlog()[-3000:], 240, 2)
    if "state stopped" not in vmlog()[-3000:]:
        say("poweroff")
        pump(10)
    try:
        dev.stdin.close()
    except OSError:
        pass
    dev.wait(30)
print("== win-agent: %d passed, %d failed" % (p, f))
sys.exit(1 if f else 0)
