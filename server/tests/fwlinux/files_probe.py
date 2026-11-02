#!/usr/bin/python3
# files_probe.py — stand-in app for tests/fwlinux/files.sh: launched by the real launcher
# (build/launch-cli) through a .desktop, it tries what a restricted app should and shouldn't be
# able to do and writes the results as JSON to ~/.cache/fwprobe/result.json (its own cache dir,
# the one place a restricted run and the test both see).
#   files_probe.py [--outside-pid=N] [--tmp-socket=PATH] [file]
import json, os, socket, subprocess, sys
home = os.environ["HOME"]
res = {}
def t(name, fn):
    try:
        fn(); res[name] = "ok"
    except OSError as e:
        res[name] = os.strerror(e.errno) if e.errno else (e.strerror or str(e))
    except Exception as e:
        res[name] = type(e).__name__
def rd(p): open(p, "rb").read(1)
def wr(p): open(p, "ab").close()
def ls(p): os.listdir(p)
t("read_documents", lambda: rd(home + "/Documents/fwlinux-secret.txt"))
t("list_documents", lambda: ls(home + "/Documents"))
t("list_home", lambda: ls(home))
t("read_firewall_json", lambda: rd(home + "/.config/axia/firewall.json"))
t("write_autostart", lambda: wr(home + "/.config/autostart/fwlinux.desktop"))
t("read_ssh", lambda: ls(home + "/.ssh"))
t("read_system", lambda: rd("/usr/share/applications/" + sorted(os.listdir("/usr/share/applications"))[0]))
t("write_tmp", lambda: wr("/tmp/fwlinux-probe-tmp"))
t("write_own_config", lambda: wr(home + "/.config/fwprobe/settings.ini"))
t("write_own_cache", lambda: wr(home + "/.cache/fwprobe/cache.bin"))
t("rename_in_own_config", lambda: (wr(home + "/.config/fwprobe/a.tmp"), os.rename(home + "/.config/fwprobe/a.tmp", home + "/.config/fwprobe/a")))
t("read_desktop_files", lambda: ls(home + "/.local/share/applications"))
rt = os.environ.get("XDG_RUNTIME_DIR", "/run/user/%d" % os.getuid())
t("write_runtime_dir", lambda: wr(rt + "/fwlinux-probe"))
def conn(p):
    s = socket.socket(socket.AF_UNIX); s.settimeout(2); s.connect(p); s.close()
args = [a for a in sys.argv[1:] if not a.startswith("--")]
outside = [a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--outside-pid=")]
# the session's sockets: none reachable except the display and the sound server's
t("connect_session_bus", lambda: conn(rt + "/bus"))
t("connect_systemd_private", lambda: conn(rt + "/systemd/private"))
t("connect_pipewire_manager", lambda: conn(rt + "/pipewire-0-manager"))
t("connect_wayland", lambda: conn(rt + "/" + os.environ.get("WAYLAND_DISPLAY", "wayland-0")))
t("connect_pipewire", lambda: conn(rt + "/pipewire-0"))
# the working directory it was started in (the .desktop's Path=): a relative connect must not reach
# the real folder hidden under the tmpfs
t("cwd_relative_bus", lambda: conn("bus"))
def no_session_pts():
    if [n for n in os.listdir("/dev/pts") if n.isdigit()]: raise OSError(0, "session terminals visible")
t("session_pts_hidden", no_session_pts)
t("read_session_shm", lambda: rd("/dev/shm/fwlinux-shm-%d" % os.getuid()))
t("write_shared_cache", lambda: wr(home + "/.cache/fontconfig/fwlinux-probe"))
if outside:
    t("outside_proc_root", lambda: conn("/proc/%s/root%s/bus" % (outside[0], rt)))
    t("outside_proc_visible", lambda: os.stat("/proc/%s" % outside[0]))
for host in [p for p in sys.argv[1:] if p.startswith("--tmp-socket=")]:
    t("connect_tmp_socket", lambda: conn(host.split("=", 1)[1]))
def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
    if r.returncode: raise OSError(0, "exit %d" % r.returncode)
marker = home + "/.cache/fwprobe/escaped"
t("systemd_run_user", lambda: run(["systemd-run", "--user", "--quiet", "touch", marker]))
t("gdbus_start_transient_unit", lambda: run(["gdbus", "call", "--session", "--dest", "org.freedesktop.systemd1",
    "--object-path", "/org/freedesktop/systemd1", "--method", "org.freedesktop.systemd1.Manager.StartTransientUnit",
    "fwlinux-escape.service", "fail", "[('ExecStart', <[('/usr/bin/touch', ['/usr/bin/touch', '%s'], false)]>)]" % marker, "[]"]))
if args:
    t("read_opened_file", lambda: rd(args[0]))
    t("write_opened_file", lambda: wr(args[0]))
    t("read_next_to_opened", lambda: rd(os.path.join(os.path.dirname(args[0]), "fwlinux-sibling.txt")))
t("write_python3_config", lambda: wr(home + "/.config/python3/x"))
t("write_local_share_applications", lambda: wr(home + "/.local/share/applications/fwlinux-evil.desktop"))
t("write_bashrc", lambda: wr(home + "/.bashrc"))
# --result-in-opened: the results go into the file it was opened with (an app with no own folders)
out = args[0] if "--result-in-opened" in sys.argv and args else home + "/.cache/fwprobe/result.json"
json.dump(res, open(out, "w"))
