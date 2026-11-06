# lc.py USER JS [TIMEOUT_S]  (run as root): runs JS (the body of an async function with cloud, cloudState, wm, lord,
# sleep, log in scope) in USER's headless session through driver.js, prints the JSON answer, exits 0 if it ran.
# JS "-" reads it from stdin. Example: lc.py e2ea 'return cloud.state().accounts.map(a => [a.name, a.state])'
import json, os, pwd, sys, time
user, src = sys.argv[1], sys.argv[2]
timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 60
if src == "-": src = sys.stdin.read()
pw = pwd.getpwnam(user)
d = os.path.join(pw.pw_dir, ".e2e")
nf = os.path.join(d, "next")
n = int(open(nf).read()) if os.path.exists(nf) else 1
open(nf, "w").write(str(n + 1))
tmp = os.path.join(d, ".cmd-%d.tmp" % n)
with open(tmp, "w") as f: f.write(src)
for p in (tmp, nf): os.chown(p, pw.pw_uid, pw.pw_gid)
os.rename(tmp, os.path.join(d, "cmd-%d.js" % n))
out = os.path.join(d, "out-%d.json" % n)
end = time.time() + timeout
while time.time() < end:
    if os.path.exists(out):
        time.sleep(0.05)
        r = json.load(open(out))
        print(json.dumps(r, indent=None))
        sys.exit(0 if r.get("ok") else 1)
    time.sleep(0.1)
print(json.dumps({"ok": False, "error": "timeout", "n": n}))
sys.exit(3)
