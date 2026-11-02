#!/usr/bin/env python3
"""Builds the LordOS demo video: records the VM while it runs a script of scenes,
with an English voice (the host's `say` command) and Spanish subtitles drawn over the video.

Usage (on the development machine, with ffmpeg and Pillow):
  python3 tools/demo/demo.py script.json output.mp4 [--vmid 117] [--voice Samantha]

The script is a list of scenes:
  { "chapter": "La Tienda",            # optional: title screen before the scene
    "en": "LordOS has its own store…",  # what the voice says
    "es": "LordOS tiene su propia tienda…",   # the subtitle
    "actions": [["key", "{f4}"], ["type", "tienda{ret}"], ["move", 640, 300], ["click", 640, 300],
                ["rclick", 547, 757], ["wheel", "down", 3], ["wait", 1.5], ["ssh", "command in the VM"]],
    "min": 4 }                          # minimum seconds
Each scene lasts at least as long as its voice. Actions go over QMP to the Proxmox host
(tools/lord-qmp-input.py, tools/lord-type.py), and recording is done by tools/demo/record.py."""
import json, os, shutil, subprocess, sys, tempfile, time
from PIL import Image, ImageDraw, ImageFont

KEY = os.path.expanduser('~/.ssh/ssh/sshkey/lord.basex.pem')
HOST = 'root@172.17.0.2'
VM_SSH = None                           # 'lord@172.17.0.51' for "ssh" actions
W, H = 1280, 800


def ssh(host, cmd, check=False):
    return subprocess.run(['ssh', '-o', 'ConnectTimeout=10', '-i', KEY, host, cmd], capture_output=True, text=True, check=check)


# Voice: Kokoro-82M (tts_kokoro.py) if LORDOS_TTS_PY (python with kokoro-onnx) and
# LORDOS_TTS_MODEL (model folder) are set and the voice is a Kokoro one (af_heart…); otherwise the host's `say`.
def voice(text, path, name):
    py, model = os.environ.get('LORDOS_TTS_PY'), os.environ.get('LORDOS_TTS_MODEL')
    if py and model and '_' in name:
        subprocess.run([py, os.path.join(os.path.dirname(__file__), 'tts_kokoro.py'), model, name, path, text], check=True)
    else:
        subprocess.run(['say', '-v', name, '-o', path, '--data-format=LEF32@48000', text], check=True)
    out = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', path], capture_output=True, text=True)
    return float(out.stdout.strip() or 0)


# Latin American keyboard (latam) seen through the keys of a US keyboard (what sendkey sends).
LATAM = {'-': '{slash}', '_': '{shift-slash}', ':': '{shift-dot}', ';': '{shift-comma}', '(': '{shift-8}', ')': '{shift-9}',
         '*': '{shift-bracket_right}', '+': '{bracket_right}', '/': '{shift-7}', '=': '{shift-0}', '?': '{shift-minus}',
         "'": '{minus}', '"': '{shift-2}', '#': '{shift-3}', '%': '{shift-5}', '&': '{shift-6}', '!': '{shift-1}', '.': '{dot}',
         ',': '{comma}', '<': '{less}', '>': '{shift-less}', '\n': '{ret}'}


def latam(text):
    return ''.join(LATAM.get(c, c) for c in text)


def act(vmid, a):
    k = a[0]
    if k == 'wait': time.sleep(a[1]); return
    if k == 'ssh' and VM_SSH: ssh(VM_SSH, a[1]); return
    if k == 'host': ssh(HOST, a[1]); return                       # command on Proxmox (qm set…, qm reset…)
    if k == 'assert':                                             # host command that must return 0; otherwise, abort
        if ssh(HOST, a[1]).returncode != 0: raise SystemExit(f'check failed: {a[1]}')
        return
    if k == 'waitfor':                                            # waits until a host command returns 0
        t0 = time.time()
        while time.time() - t0 < a[2] and ssh(HOST, a[1]).returncode != 0: time.sleep(2)
        return
    if k in ('key', 'type', 'typela'):
        txt = latam(a[1]) if k == 'typela' else a[1]
        txt = txt.replace("'", "'\\''")
        ssh(HOST, f"python3 /root/lord-type.py {vmid} '{txt}'"); return
    if k == 'move': mouse = f'move {a[1]} {a[2]}'
    elif k == 'click': mouse = f'move {a[1]} {a[2]} down up'
    elif k == 'rclick': mouse = f'move {a[1]} {a[2]} rdown rup'
    elif k == 'dclick': mouse = f'move {a[1]} {a[2]} down up down up'
    elif k == 'drag': mouse = f'move {a[1]} {a[2]} down sleep 0.2 move {a[3]} {a[4]} sleep 0.2 up'
    elif k == 'wheel': mouse = ' '.join([f'wheel{a[1]}'] * (a[2] if len(a) > 2 else 1))
    else: raise ValueError(f'unknown action: {a}')
    if mouse: ssh(HOST, f'python3 /root/lord-qmp-input.py {vmid} {W} {H} {mouse}')


def font(size, bold=False):
    for p in ('/System/Library/Fonts/Supplemental/Arial Bold.ttf' if bold else '/System/Library/Fonts/Supplemental/Arial.ttf',
              '/Library/Fonts/Arial Unicode.ttf', '/System/Library/Fonts/Helvetica.ttc'):
        if os.path.exists(p): return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def wrap(draw, text, f, maxw):
    lines, cur = [], ''
    for word in text.split():
        t = (cur + ' ' + word).strip()
        if draw.textlength(t, font=f) > maxw and cur: lines.append(cur); cur = word
        else: cur = t
    if cur: lines.append(cur)
    return lines


def subtitle(img, text):
    if not text: return img
    img = img.convert('RGBA')
    over = Image.new('RGBA', img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    f = font(26)
    lines = wrap(d, text, f, img.width - 220)
    lh = 34
    y = img.height - 112 - lh * len(lines)          # above the Dock
    for ln in lines:
        tw = d.textlength(ln, font=f)
        x = (img.width - tw) / 2
        d.rounded_rectangle((x - 14, y - 4, x + tw + 14, y + lh - 2), radius=8, fill=(0, 0, 0, 170))
        d.text((x, y), ln, font=f, fill=(255, 255, 255, 255))
        y += lh
    return Image.alpha_composite(img, over).convert('RGB')


def title_card(text, sub, path):
    img = Image.new('RGB', (W, H), (18, 14, 40))
    d = ImageDraw.Draw(img)
    for i in range(H):                                            # violet gradient
        c = int(30 + 40 * i / H)
        d.line([(0, i), (W, i)], fill=(c, 18, 70 + int(50 * i / H)))
    mark = os.path.join(os.path.dirname(__file__), '../../assets/icons/axia-mark.png')
    if os.path.exists(mark):
        ic = Image.open(mark).convert('RGBA').resize((120, 120))
        img.paste(ic, ((W - 120) // 2, 200), ic)
    f1, f2 = font(54, True), font(26)
    tw = d.textlength(text, font=f1)
    d.text(((W - tw) / 2, 350), text, font=f1, fill=(255, 255, 255))
    if sub:
        tw = d.textlength(sub, font=f2)
        d.text(((W - tw) / 2, 430), sub, font=f2, fill=(220, 210, 255))
    img.save(path)


def main():
    global VM_SSH
    args = sys.argv[1:]
    script, out = args[0], args[1]
    vmid = args[args.index('--vmid') + 1] if '--vmid' in args else '117'
    vname = args[args.index('--voice') + 1] if '--voice' in args else ('af_heart' if os.environ.get('LORDOS_TTS_MODEL') else 'Samantha')
    VM_SSH = args[args.index('--vm-ssh') + 1] if '--vm-ssh' in args else None
    scenes = json.load(open(script))
    if '--rehearse' in args:                                      # rehearsal: no voice or recording, one screenshot per scene
        first = int(args[args.index('--from') + 1]) if '--from' in args else 0
        steps = '--steps' in args                                 # one screenshot after each action
        for i, s in enumerate(scenes[first:], first):
            for j, a in enumerate(s.get('actions', [])):
                act(vmid, a)
                if steps and a[0] not in ('wait', 'move'):
                    time.sleep(0.7)
                    ssh(HOST, f'echo "screendump /tmp/reh.png -f png" | qm monitor {vmid} >/dev/null')
                    subprocess.run(['scp', '-q', '-i', KEY, f'{HOST}:/tmp/reh.png', f'{out}-{i:02d}-{j:02d}.png'])
            time.sleep(1.0)
            for a in s.get('actions', []):
                if a[0] == 'wait' and s.get('speed'): pass
            ssh(HOST, f'echo "screendump /tmp/reh.png -f png" | qm monitor {vmid} >/dev/null')
            subprocess.run(['scp', '-q', '-i', KEY, f'{HOST}:/tmp/reh.png', f'{out}-{i:02d}.png'])
            print(i, s.get('chapter') or s.get('es', '')[:40], flush=True)
        return
    tmp = tempfile.mkdtemp(prefix='lordos-demo-')
    # 1. voices
    for i, s in enumerate(scenes):
        s['voice'] = os.path.join(tmp, f'v{i:03d}.wav')
        s['vdur'] = voice(s['en'], s['voice'], vname) if s.get('en') else 0
    # 2. recording
    rec = f'/dev/shm/lordos-rec-{vmid}'
    ssh(HOST, f'rm -rf {rec}; mkdir -p {rec}')
    subprocess.run(['scp', '-q', '-i', KEY, os.path.join(os.path.dirname(__file__), 'record.py'), f'{HOST}:/root/lordos-record.py'], check=True)
    recp = subprocess.Popen(['ssh', '-i', KEY, HOST, f'python3 /root/lordos-record.py {vmid} {rec} 6'])
    time.sleep(1.5)
    t0 = time.time()
    timeline = []
    try:
        for s in scenes:
            start = time.time() - t0
            for a in s.get('actions', []): act(vmid, a)
            need = max(s.get('min', 0), s['vdur'] + 0.6)
            while time.time() - t0 - start < need: time.sleep(0.05)
            timeline.append((start, time.time() - t0))
    finally:                                                      # the recorder always stops
        ssh(HOST, f'touch {rec}/STOP'); recp.wait()
    # 3. frames: download, subtitle
    local = os.path.join(tmp, 'frames')
    os.makedirs(local)
    subprocess.run(f"ssh -i {KEY} {HOST} 'tar cf - -C {rec} .' | tar xf - -C {local}", shell=True, check=True)
    ssh(HOST, f'rm -rf {rec}')
    frames = [ln.split() for ln in open(os.path.join(local, 'frames.txt'))]
    ft = [(float(t) - 1.5, name) for t, name in frames]           # same zero as the timeline
    concat, aud = [], []
    out_t = 0.0                                                   # time in the final video
    seg = 0
    for i, s in enumerate(scenes):
        a, b = timeline[i]
        if s.get('chapter'):                                      # title screen (3 s)
            p = os.path.join(tmp, f'title{i:03d}.png')
            title_card(s['chapter'], s.get('chapter_es', ''), p)
            concat.append((p, 3.0)); out_t += 3.0
        aud.append((out_t, s['voice']) if s['vdur'] else None)
        inside = [(t, n) for t, n in ft if a <= t < b] or [min(ft, key=lambda x: abs(x[0] - a))]
        # "speed": the scene is sped up (installation, downloads), but never lasts less than its voice
        sp = max(1.0, min(s.get('speed', 1.0), (b - a) / max(1.0, s['vdur'] + 0.6)))
        if sp > 1:                                                # skipped frames to avoid bloat
            step = max(1, int(sp / 2))
            inside = inside[::step]
        s['outdur'] = 0.0
        for j, (t, n) in enumerate(inside):
            nxt = inside[j + 1][0] if j + 1 < len(inside) else b
            p = os.path.join(tmp, f's{seg:06d}.png'); seg += 1
            im = Image.open(os.path.join(local, n)).convert('RGB')
            if im.size != (W, H):                                 # boot (BIOS, grub): another resolution
                bg = Image.new('RGB', (W, H)); im.thumbnail((W, H)); bg.paste(im, ((W - im.width) // 2, (H - im.height) // 2)); im = bg
            subtitle(im, s.get('es', '')).save(p)
            dur = max(0.04, (nxt - t) / sp)
            concat.append((p, dur)); out_t += dur; s['outdur'] += dur
    with open(os.path.join(tmp, 'concat.txt'), 'w') as f:
        for p, d in concat: f.write(f"file '{p}'\nduration {d:.3f}\n")
        f.write(f"file '{concat[-1][0]}'\n")
    # 4. audio: each voice in its place
    inputs, filt, labels = [], [], []
    for k, v in enumerate([x for x in aud if x]):
        inputs += ['-i', v[1]]
        ms = int(v[0] * 1000)
        filt.append(f'[{k + 1}:a]adelay={ms}|{ms}[a{k}]'); labels.append(f'[a{k}]')
    fc = ';'.join(filt + [f"{''.join(labels)}amix=inputs={len(labels)}:normalize=0[aout]"]) if labels else None
    srt = os.path.join(tmp, 'es.srt')
    with open(srt, 'w') as f:                                     # separate subtitle track (can be turned off)
        tcur, idx = 0.0, 1
        for i, s in enumerate(scenes):
            if s.get('chapter'): tcur += 3.0
            d = s.get('outdur', 0.0)
            fmt = lambda x: f'{int(x // 3600):02d}:{int(x % 3600 // 60):02d}:{int(x % 60):02d},{int(x * 1000 % 1000):03d}'
            if s.get('es'): f.write(f"{idx}\n{fmt(tcur)} --> {fmt(tcur + d)}\n{s['es']}\n\n"); idx += 1
            tcur += d
    cmd = ['ffmpeg', '-y', '-f', 'concat', '-safe', '0', '-i', os.path.join(tmp, 'concat.txt')] + inputs + ['-i', srt]
    if fc: cmd += ['-filter_complex', fc, '-map', '0:v', '-map', '[aout]']
    else: cmd += ['-map', '0:v']
    cmd += ['-map', f'{len(labels) + 1}:s', '-vf', 'fps=30,format=yuv420p', '-c:v', 'libx264', '-preset', 'slow', '-crf', '20',
            '-c:a', 'aac', '-b:a', '160k', '-c:s', 'mov_text', '-metadata:s:s:0', 'language=spa', '-movflags', '+faststart', out]
    subprocess.run(cmd, check=True)
    shutil.rmtree(tmp, ignore_errors=True)
    print(out)


if __name__ == '__main__':
    main()
