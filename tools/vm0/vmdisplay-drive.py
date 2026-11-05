#!/usr/bin/env python3
# Phase 0 test driver for vmdisplay-proto (docs/VIRTUALIZATION.md section 3.2), amd64 or arm64 under KVM.
# Starts QEMU paused with -display dbus,p2p=yes, attaches vmdisplay-proto through its own QMP socket,
# continues the guest, logs in on the graphic console by typing through org.qemu.Display1.Keyboard,
# and measures time to first frame, update rate, latency and CPU while the console scrolls.
# Usage: vmdisplay-drive.py <image.qcow2> <proto-binary> <out-dir> <tag> [--no-map] [--coalesce HZ] [--gl]
#        [--kernel VMLINUZ --initrd INITRD --root ROOT=...]   (arm64: boot the guest kernel directly, no firmware;
#        QEMU 10.0 aborts in dbus_scanout_texture when the firmware's virtio-gpu reset reaches a GL console)
import json, os, platform, re, secrets, socket, statistics, subprocess, sys, threading, time

img, proto, outdir, tag = sys.argv[1:5]
no_map = '--no-map' in sys.argv
gl = '--gl' in sys.argv
coalesce = sys.argv[sys.argv.index('--coalesce') + 1] if '--coalesce' in sys.argv else None
def opt(name): return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else None
kernel, initrd, root = opt('--kernel'), opt('--initrd'), opt('--root')
os.makedirs(outdir, exist_ok=True)
work = os.path.join(outdir, 'run-' + tag)
os.makedirs(work, exist_ok=True)
over = os.path.join(work, 'overlay.qcow2')
qmp_path = os.path.join(work, 'qmp.sock')
qmp_disp = os.path.join(work, 'qmp-disp.sock')
ser_path = os.path.join(work, 'serial.sock')
qtrace = os.path.join(work, 'qemu-trace.log')
ptrace = os.path.join(work, 'proto-trace.log')
for p in (over, qmp_path, qmp_disp, ser_path, qtrace, ptrace):
    if os.path.exists(p): os.unlink(p)
subprocess.run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-F', 'qcow2', '-b', os.path.abspath(img), over], check=True)
pw = secrets.token_hex(8)  # throwaway root password for this run's overlay only

kvm = os.open('/dev/kvm', os.O_RDWR | os.O_CLOEXEC)
os.set_inheritable(kvm, True)
if platform.machine() == 'aarch64':
    # virt has no built-in keyboard: Display1.Keyboard needs a guest input device. AAVMF drives virtio-gpu-pci.
    vars_ = os.path.join(work, 'vars.fd')
    subprocess.run(['truncate', '-s', '0', vars_], check=True); subprocess.run(['truncate', '-s', '64M', vars_], check=True)
    base = ['qemu-system-aarch64', '-M', 'virt,dump-guest-core=off', '-cpu', 'host']
    if kernel:  # no firmware, so no SMBIOS: the same credentials go on the kernel command line
        creds = ' '.join(f'systemd.set_credential={c}' for c in
                         ('firstboot.locale:C.UTF-8', 'firstboot.keymap:us', 'firstboot.timezone:UTC', f'passwd.plaintext-password.root:{pw}'))
        base += ['-kernel', kernel, '-initrd', initrd, '-append', f'{root} ro console=tty0 console=ttyAMA0 {creds}']
    else:
        base += ['-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd',
                 '-drive', f'if=pflash,format=raw,file={vars_}']
    gpu = ['-device', 'virtio-gpu-gl-pci' if gl else 'virtio-gpu-pci', '-device', 'virtio-keyboard-pci']
else:
    base = ['qemu-system-x86_64', '-M', 'q35,dump-guest-core=off', '-cpu', 'host']
    gpu = ['-device', 'virtio-vga-gl' if gl else 'virtio-vga']
cmd = base + ['-add-fd', f'fd={kvm},set=1,opaque=kvm', '-accel', 'kvm,device=/dev/fdset/1',
       '-smp', '2', '-m', '2048', '-nodefaults', '-no-user-config', '-S'] + gpu + [
       '-display', 'dbus,p2p=yes,gl=on' if gl else 'dbus,p2p=yes',
       '-drive', f'if=virtio,format=qcow2,file={over},discard=unmap',
       '-netdev', 'user,id=n0', '-device', 'virtio-net-pci,netdev=n0',
       '-chardev', f'socket,id=ser0,path={ser_path},server=on,wait=off', '-serial', 'chardev:ser0',
       '-smbios', 'type=11,value=io.systemd.credential:firstboot.locale=C.UTF-8',
       '-smbios', 'type=11,value=io.systemd.credential:firstboot.keymap=us',
       '-smbios', 'type=11,value=io.systemd.credential:firstboot.timezone=UTC',
       '-smbios', f'type=11,value=io.systemd.credential:passwd.plaintext-password.root={pw}',
       '-qmp', f'unix:{qmp_path},server=on,wait=off',
       '-qmp', f'unix:{qmp_disp},server=on,wait=off',
       '-msg', 'timestamp=on', '-trace', 'dbus_update', '-D', qtrace,
       '-sandbox', 'on,obsolete=deny,elevateprivileges=deny,spawn=deny,resourcecontrol=deny']
t_exec = time.time()
q = subprocess.Popen(cmd, pass_fds=(kvm,), stderr=subprocess.PIPE, text=True)
os.close(kvm)
res = {'tag': tag, 'map': not no_map, 'gl': gl, 'coalesce_hz': coalesce}

def wait_sock(p):
    for _ in range(200):
        if os.path.exists(p): return True
        if q.poll() is not None: return False
        time.sleep(0.02)
    return False

def fail(why):
    res['error'] = why
    try: res['qemu_stderr'] = q.stderr.read()[-1500:] if q.poll() is not None else ''
    except Exception: pass
    if globals().get('buf'): res['serial_tail'] = buf[-1500:].decode(errors='replace')
    res['qemu_exit'] = q.poll(); res['died_after_cont_s'] = round(time.time() - t_cont, 1) if 't_cont' in globals() else None
    print(json.dumps(res)); sys.exit(1)

if not wait_sock(qmp_path): fail('qemu did not start')
s = socket.socket(socket.AF_UNIX); s.connect(qmp_path); f = s.makefile('rw')
def qmp(c, **a):
    f.write(json.dumps({'execute': c, **({'arguments': a} if a else {})}) + '\n'); f.flush()
    while True:
        m = json.loads(f.readline())
        if 'return' in m or 'error' in m: return m
json.loads(f.readline()); qmp('qmp_capabilities')
res['qemu_version'] = qmp('query-version')['return']['package']

# --- attach the display helper ---
pargs = [proto, qmp_disp, '--trace', ptrace, '--introspect', os.path.join(work, 'introspect.xml')]
if no_map: pargs.append('--no-map')
if coalesce: pargs += ['--coalesce', coalesce]
t_attach = time.time()
p = subprocess.Popen(pargs, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(os.path.join(work, 'proto.err'), 'w'), text=True, bufsize=1)
events = []; ev_lock = threading.Condition()
def reader():
    for line in p.stdout:
        try: m = json.loads(line)
        except ValueError: continue
        m['_t'] = time.time()
        with ev_lock: events.append(m); ev_lock.notify_all()
threading.Thread(target=reader, daemon=True).start()
def wait_ev(pred, timeout=30):
    end = time.time() + timeout
    with ev_lock:
        while True:
            for m in events:
                if pred(m) and not m.get('_used'): m['_used'] = True; return m
            left = end - time.time()
            if left <= 0: return None
            ev_lock.wait(left)
def proto_cmd(line, want=None, timeout=10):
    p.stdin.write(line + '\n'); p.stdin.flush()
    if want: return wait_ev(lambda m: m.get('cmd') == want, timeout)

lst = wait_ev(lambda m: m.get('ev') == 'listener', 15)
if not lst: p.kill(); fail('listener registration failed')
conn = [m for m in events if m.get('ev') == 'connected']
res['attach_ms'] = round((lst['_t'] - t_attach) * 1000, 1)
res['console'] = conn[0] if conn else None

t_cont = time.time(); qmp('cont')
fs = wait_ev(lambda m: m.get('ev') == 'scanout', 30)
fu = wait_ev(lambda m: m.get('ev') == 'first_update', 30)
res['first_scanout_after_cont_ms'] = round((fs['rt'] - t_cont) * 1000, 1) if fs else None
res['first_update_after_cont_ms'] = round((fu['rt'] - t_cont) * 1000, 1) if fu else None
res['first_scanout_after_exec_ms'] = round((fs['rt'] - t_exec) * 1000, 1) if fs else None

# --- serial: wait for the login prompt, learn whether root needs the password ---
ser = socket.socket(socket.AF_UNIX); ser.connect(ser_path); ser.settimeout(0.5)
buf = b''
def ser_wait(pat, timeout):
    global buf
    end = time.time() + timeout
    while time.time() < end:
        try:
            d = ser.recv(65536)
            if d: buf += d
        except socket.timeout: pass
        if re.search(pat, buf[-4000:]): return True
    return False
if not ser_wait(rb'login: ', 180): fail('no login prompt on serial')
res['login_prompt_after_cont_s'] = round(time.time() - t_cont, 1)
ser.sendall(b'root\r')
ser_wait(rb'Password: |# $', 10)
needs_pw = b'Password:' in buf[-300:]
res['root_needs_password'] = needs_pw
if needs_pw: ser.sendall(pw.encode() + b'\r'); ser_wait(rb'# $', 15)
ser.sendall(b'exit\r')
time.sleep(2)

shots = os.path.join(outdir, 'shots'); os.makedirs(shots, exist_ok=True)
proto_cmd(f'png {shots}/{tag}-1-login-prompt.png', 'png')

# --- keyboard: log in on tty1 ---
proto_cmd('type root\\n', 'type'); time.sleep(2.0)
proto_cmd(f'png {shots}/{tag}-2-after-root.png', 'png')
if needs_pw:
    proto_cmd(f'type {pw}\\n', 'type'); time.sleep(3.0)
proto_cmd('type uname -a; cat /etc/debian_version\\n', 'type'); time.sleep(2.0)
proto_cmd(f'png {shots}/{tag}-3-logged-in.png', 'png')

tck = os.sysconf('SC_CLK_TCK')
def cpu(pid):
    v = open(f'/proc/{pid}/stat').read().rsplit(')', 1)[1].split()
    return (int(v[11]) + int(v[12])) / tck
def window(name, secs=10.0):
    proto_cmd('reset', 'reset')
    t0 = time.time(); cq0 = cpu(q.pid); cp0 = cpu(p.pid)
    time.sleep(secs)
    st = proto_cmd('stats', 'stats')
    dt = time.time() - t0
    st['qemu_cpu_pct'] = round(100 * (cpu(q.pid) - cq0) / dt, 1)
    st['proto_cpu_pct_proc'] = round(100 * (cpu(p.pid) - cp0) / dt, 1)
    st['t0'] = t0; st['t1'] = t0 + dt
    for k in ('_t', '_used', 'cmd'): st.pop(k, None)
    res[name] = st
window('idle')
proto_cmd('type yes\\n', 'type'); time.sleep(1.0)
window('yes')
proto_cmd(f'png {shots}/{tag}-4-yes.png', 'png')
proto_cmd('ctrl 0x2e', 'key'); time.sleep(1.0)
proto_cmd('type clear; for i in $(seq 400); do dmesg; done\\n', 'type'); time.sleep(1.0)
window('dmesg')
proto_cmd(f'png {shots}/{tag}-5-dmesg.png', 'png')
proto_cmd('ctrl 0x2e', 'key'); time.sleep(0.5)

# --- latency: QEMU's dbus_update trace (gettimeofday) against our receipt time (CLOCK_REALTIME) ---
def lat(t0, t1):
    qe = []
    for line in open(qtrace, errors='replace'):
        m = re.match(r'\d+@(\d+)\.(\d+):dbus_update x=(-?\d+), y=(-?\d+), w=(-?\d+), h=(-?\d+)', line)
        if m: qe.append((int(m[1]) + int(m[2]) / 1e6, tuple(int(m[i]) for i in range(3, 7))))
    pe = []
    for line in open(ptrace):
        a = line.split()
        if a[1] in ('Update', 'UpdateMap'): pe.append((float(a[0]), tuple(int(x) for x in a[2:6])))
    # Walk both lists backwards: each receipt pairs with the latest earlier, unused QEMU entry for the same
    # rectangle. QEMU drops queued display messages on a new scanout, so forward pairing would drift.
    out = []; j = len(qe) - 1
    for t, r in reversed(pe):
        while j >= 0 and (qe[j][0] > t or qe[j][1] != r): j -= 1
        if j < 0: break
        if t0 <= t <= t1: out.append((t - qe[j][0]) * 1e6)
        j -= 1
    out.sort()
    if not out: return None
    return {'n': len(out), 'median_us': round(statistics.median(out)), 'p95_us': round(out[int(len(out) * 0.95) - 1 if len(out) > 1 else 0]), 'max_us': round(out[-1])}
res['latency_yes'] = lat(res['yes']['t0'], res['yes']['t1'])
res['latency_dmesg'] = lat(res['dmesg']['t0'], res['dmesg']['t1'])
res['dmabuf_updates'] = {w: res[w].get('update_dmabuf') for w in ('idle', 'yes', 'dmesg')}
if gl:  # the prototype does not import dmabufs, so ask QEMU for the picture it holds
    res['screendump'] = qmp('screendump', filename=os.path.join(shots, tag + '-6-screendump.ppm'))
res['events'] = sorted({m['ev'] for m in events if 'ev' in m})
res['dmabuf_events'] = [m for m in events if m.get('ev') == 'scanout_dmabuf'][:3]
res['map_fd'] = [{k: v for k, v in m.items() if not k.startswith('_')} for m in events if m.get('ev') == 'scanout_map_fd'][:3]
res['scanouts'] = [{k: v for k, v in m.items() if k in ('kind', 'w', 'h')} for m in events if m.get('ev') == 'scanout']
res['cursors'] = [{k: v for k, v in m.items() if not k.startswith('_')} for m in events if m.get('ev') == 'cursor'][:3]
res['rejects'] = len([m for m in events if m.get('ev') == 'reject'])
try: res['qemu_rss_mb'] = int([l for l in open(f'/proc/{q.pid}/status') if l.startswith('VmRSS')][0].split()[1]) // 1024
except Exception: pass
try: res['proto_rss_kb'] = int([l for l in open(f'/proc/{p.pid}/status') if l.startswith('VmRSS')][0].split()[1])
except Exception: pass

proto_cmd('quit'); p.wait(timeout=10)
qmp('quit'); q.wait(timeout=30)
res['exit'] = q.returncode
for k in ('t0', 't1'):
    for w in ('idle', 'yes', 'dmesg'): res[w].pop(k, None)
os.unlink(over)
if os.path.exists(os.path.join(work, 'vars.fd')): os.unlink(os.path.join(work, 'vars.fd'))
print(json.dumps(res))
