#!/usr/bin/env python3
"""Build LordOS Studio's background loops (the videos of assets/studio/backgrounds.json) from tools/studio/bg-sources.json.

Every loop is made at 3840x2160 from an original at least that big (never upscaled), plus a 1920x1080 variant. The
catalog's pictures are not built here: they are carried over from assets/studio/backgrounds.json as they are, and
their files are not touched. Inputs are the downloads kept outside the repository (one folder per source):
  <src>/commons4k/<key>.<ext>          Wikimedia Commons originals (key = sha1(title)[:12])
  <src>/procedural4k/<item>.mkv        renders of tools/studio/gen-studio-loops.py (3840x2160)

Outputs:
  <stage>/videos/<id>.{2160,1080}.h264.mp4, <stage>/videos/README.md (credits), <stage>/videos/MANIFEST.txt (size
  and SHA-256 of every loop), assets/studio/backgrounds/<id>.thumb.jpg (640x360) for each loop,
  assets/studio/backgrounds.json

The downloads are untrusted data: run with `python3 -I`, never from inside the download folders.

usage: python3 -I tools/studio/build-backgrounds.py --src DIR --stage DIR [--force] [--only id,id] [--jobs N]
"""
import argparse
import glob
import hashlib
import io
import json
import os
import subprocess
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASE = "https://raw.githubusercontent.com/lordbasex/LordOS-wallpapers/main/studio"
OW, OH = 3840, 2160
VID_SIZES = [("2160", 3840, 2160), ("1080", 1920, 1080)]
TW, TH = 640, 360
THUMB_Q = 85
MAX_FILE = 100 * 1000 * 1000      # E-S4: studio-bg-get refuses anything bigger (bytes)
CATEGORIES = ["office", "home", "studio", "nature", "city", "abstract", "space"]


def quantize(a, seed):
    """sRGB float [0,1] (H, W, 3) -> uint8 with triangular dither of one level (fixed seed: reproducible)."""
    rng = np.random.default_rng(seed)
    n = rng.random(a.shape[:2] + (1,), np.float32) - rng.random(a.shape[:2] + (1,), np.float32)
    return np.clip(np.floor(a * 255.0 + 0.5 + n), 0, 255).astype(np.uint8)


def area_down(a, w, h):
    """Exact area average when the factor is whole, else a Lanczos resample per channel (float)."""
    H0, W0 = a.shape[:2]
    if W0 % w == 0 and H0 % h == 0 and W0 // w == H0 // h:
        f = W0 // w
        return a.reshape(h, f, w, f, a.shape[2]).mean((1, 3))
    return resize_f(a, w, h)


def resize_f(a, w, h, flt=Image.LANCZOS):
    return np.stack([np.asarray(Image.fromarray(np.ascontiguousarray(a[..., c], np.float32), "F").resize((w, h), flt))
                     for c in range(a.shape[2])], -1)


def to_float(img):
    return np.asarray(img.convert("RGB")).astype(np.float32) / 255.0


def save_jpeg(u8, path, q):
    tmp = path + ".part"
    Image.fromarray(u8).save(tmp, "JPEG", quality=q, optimize=True, progressive=False, subsampling="4:2:0")
    os.replace(tmp, path)




# ------------------------------------------------------------------ videos

def probe(path):
    j = json.loads(subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                   "stream=width,height,r_frame_rate,codec_name", "-of", "json", path],
                                  capture_output=True, text=True, check=True).stdout)["streams"][0]
    n, d = (int(x) for x in j["r_frame_rate"].split("/"))
    return j["width"], j["height"], n, d


def out_rate(n, d):
    """Keep the source rate up to 30 fps; halve higher rates (60 -> 30, 59.94 -> 29.97). Never interpolate."""
    if n / d <= 30.01:
        return n, d
    if n / d / 2 >= 23.9:
        return (n, 2 * d) if n % 2 else (n // 2, d)
    return 30, 1


X264 = ["-c:v", "libx264", "-profile:v", "high", "-preset", "slower", "-pix_fmt", "yuv420p", "-an", "-sn", "-dn",
        "-map_metadata", "-1", "-map_chapters", "-1", "-movflags", "+faststart", "-fflags", "+bitexact",
        "-flags:v", "+bitexact", "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709",
        "-color_range", "tv"]
# aq-mode 3 and no deblocking of fine noise keep the renders' dither (no banding in slow gradients)
RATE = {2160: ["-crf", "18", "-maxrate", "45M", "-bufsize", "90M", "-level:v", "5.1",
               "-x264-params", "threads=12:aq-mode=3:aq-strength=0.9:deblock=-1,-1"],
        1080: ["-crf", "18", "-maxrate", "14M", "-bufsize", "28M", "-level:v", "4.2",
               "-x264-params", "threads=8:aq-mode=3:aq-strength=0.9:deblock=-1,-1"]}


def video_source(it, src):
    o = it["origin"]
    if o["type"] == "procedural":
        return os.path.join(src, "procedural4k", o["item"] + ".mkv")
    cands = [c for c in glob.glob(os.path.join(src, "commons4k", o["localKey"] + ".*")) if not c.endswith((".part", ".hdr"))]
    if len(cands) != 1:
        raise SystemExit(f"{it['id']}: source file not found for {o['localKey']}")
    return cands[0]


def build_video(it, src, stage, force):
    o, p = it["origin"], it["process"]
    path = video_source(it, src)
    w, h, n, d = probe(path)
    orig = {"w": w, "h": h}
    crop = p.get("crop")
    if crop:
        cx, cy, cw, ch = crop
        w, h = cw, ch
    if w < OW or h < OH or abs(w / h - 16 / 9) > 0.02:
        raise SystemExit(f"{it['id']}: the source ({w}x{h}) isn't a 16:9 picture of at least {OW}x{OH}")
    on, od = out_rate(n, d)
    secs = p["seconds"]
    outs = {s: os.path.join(stage, "videos", f"{it['id']}.{s}.h264.mp4") for s, _, _ in VID_SIZES}
    if all(os.path.exists(f) for f in outs.values()) and not force:
        return outs, on / od, secs, orig
    # scaled in 16 bits per channel; the step to 8 bits is dithered by swscale (error diffusion)
    proc = o["type"] == "procedural"
    pre = (f"crop={cw}:{ch}:{cx}:{cy}," if crop else "")
    pre += f"scale={OW}:{OH}:flags=lanczos,fps={on}/{od},setsar=1,format=yuv444p16le" if not proc else f"fps={on}/{od},setsar=1"
    if proc:
        graph = f"[0:v]{pre},trim=duration={secs},setpts=PTS-STARTPTS[o]"
        inp = ["-i", path]
    else:
        # The clip [start, start+secs] is closed by crossfading the part after its end over its first xf seconds,
        # so the last frame flows into the first (and the start is chosen where those two look alike: tools/studio/pick-loop-start.py).
        xf = p.get("crossfade", 1.0)
        graph = (f"[0:v]{pre},setpts=PTS-STARTPTS,split[a][b];"
                 f"[a]trim=start=0:end={secs},setpts=PTS-STARTPTS,fps={on}/{od}[A];"
                 f"[b]trim=start={secs}:end={secs + xf},setpts=PTS-STARTPTS,fps={on}/{od}[B];"
                 f"[B][A]xfade=transition=fade:duration={xf}:offset=0[o]")
        inp = ["-ss", str(p["start"]), "-t", str(secs + xf + 0.3), "-i", path]
    # Our renders are smooth gradients: the encoder would flatten their one-level dither into blocky steps, so they get a
    # light film grain on luma (deterministic: the filter's fixed seed) and x264's grain tuning keeps it. Camera clips have their own.
    # (They are already 3840x2160 and 8 bits with their own dither, so they skip the 16-bit path.)
    grain = ",noise=c0s=3:c0f=t+u" if proc else ""
    dith = "" if proc else ":sws_dither=ed"
    graph += (f";[o]split[o1][o2];[o1]scale=out_range=tv{dith},format=yuv420p{grain}[o2160];"
              f"[o2]scale={VID_SIZES[1][1]}:{VID_SIZES[1][2]}:flags=lanczos+accurate_rnd{dith},format=yuv420p{grain}[o1080]")
    maps = {"2160": "[o2160]", "1080": "[o1080]"}
    cmd = ["ffmpeg", "-nostdin", "-v", "error", "-y", "-threads", "16"] + inp + ["-filter_complex", graph]
    tune = ["-tune", "grain"] if grain else []
    for s, _, hh in VID_SIZES:
        rate = RATE[hh][:-2] if proc else RATE[hh]          # the grain tuning's own AQ and deblocking for our renders
        cmd += ["-map", maps[s], "-t", str(secs), "-r", f"{on}/{od}", "-g", str(int(round(2 * on / od)))] + X264 + tune + rate + [outs[s] + ".part.mp4"]
    subprocess.run(cmd, check=True)
    for s in outs:
        if os.path.getsize(outs[s] + ".part.mp4") > MAX_FILE:
            raise SystemExit(f"{it['id']}: {s} is over {MAX_FILE} bytes")
        os.replace(outs[s] + ".part.mp4", outs[s])
    return outs, on / od, secs, orig


def frame_at(path, t):
    r = subprocess.run(["ffmpeg", "-v", "error", "-ss", str(t), "-i", path, "-frames:v", "1", "-f", "image2pipe", "-c:v", "png", "-"],
                       capture_output=True, check=True)
    return Image.open(io.BytesIO(r.stdout)).convert("RGB")


# ------------------------------------------------------------------ catalog

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def described(it):
    p = it["process"]
    if p["op"] == "encode":
        return f"Procedural render at 3840x2160, {p['seconds']} s loop"
    did = f"Trimmed {p['start']}-{round(p['start'] + p['seconds'], 3)} s, crossfaded into a loop ({p.get('crossfade', 1.0)} s)"
    if p.get("crop"):
        did += ", cropped to 16:9"
    return did + ", scaled, audio removed"


def readme(items, stage):
    L = ["# LordOS Studio background loops", "",
         "The loops of the LordOS Studio camera backgrounds. LordOS downloads these files on demand; the catalog is",
         "`assets/studio/backgrounds.json` in the LordOS repository, and `tools/studio/bg-sources.json` records where each",
         "file came from and how it was made (`tools/studio/build-backgrounds.py` rebuilds them from the originals).", "",
         "Every file keeps the license of its source. Every loop comes in two sizes, 3840x2160 (`.2160.`) and 1920x1080",
         "(`.1080.`), both made from an original at least 3840x2160; nothing is upscaled. They are H.264 (High profile) MP4",
         "without audio, closed into a seamless loop (the part after the clip's end is crossfaded over its first seconds,",
         "from a start where the two ends look alike: `tools/studio/pick-loop-start.py`). Our own renders (author LordOS) are",
         "MIT licensed and made by `tools/studio/gen-studio-loops.py`. `MANIFEST.txt` lists the size and SHA-256 of every file.", "",
         "| Files | Title | Author | License | Source | Original | What we did |", "|---|---|---|---|---|---|---|"]
    for it in items:
        lic = f"[{it['license']}]({it['licenseUrl']})" if it["licenseUrl"] else it["license"]
        files = ", ".join(f"`{os.path.basename(v['file'])}`" for v in it["variants"])
        og = it["origin"].get("original") or {}
        orig = f"{og.get('w')}x{og.get('h')}" if og else ""
        author = it.get("credit") or it["author"]
        L.append(f"| {files} | {it['name']} | {author} | {lic} | {it['source']} | {orig} | {described(it)} |")
    L += ["", "Wikimedia Commons files: license as stated on each file page; CC BY and CC BY-SA files require the attribution",
          "shown in the Author column (the catalog's `credit` field). CC BY-SA files stay under CC BY-SA.", ""]
    with open(os.path.join(stage, "videos", "README.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(L))


def manifest(items, stage):
    L = ["# size sha256 file (LordOS Studio background loops; the catalog is assets/studio/backgrounds.json)"]
    total = n = 0
    for it in items:
        for v in it["variants"]:
            L.append(f"{v['size']} {v['sha256']} {v['file']}")
            total += v["size"]
            n += 1
    L.append(f"# {n} files, {total} bytes")
    with open(os.path.join(stage, "videos", "MANIFEST.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")


def build_one(args):
    it, src, stage, force = args
    thumbs = os.path.join(REPO, "assets/studio/backgrounds")
    rec = {k: it[k] for k in ("id", "kind", "category", "name", "author", "license", "licenseUrl", "source")}
    if "credit" in it:
        rec["credit"] = it["credit"]
    outs, fps, secs, orig = build_video(it, src, stage, force)
    rec["variants"] = [{"name": f"{n}.h264", "codec": "h264", "w": w, "h": h, "fps": round(fps, 3), "seconds": secs,
                        "file": f"videos/{it['id']}.{n}.h264.mp4", "size": os.path.getsize(outs[n]), "sha256": sha256(outs[n])}
                       for n, w, h in VID_SIZES]
    thumb = os.path.join(thumbs, it["id"] + ".thumb.jpg")
    if force or not os.path.exists(thumb):
        m = to_float(frame_at(outs["2160"], secs * 0.5))
        save_jpeg(quantize(area_down(m, TW, TH), 7), thumb, THUMB_Q)
    for v in rec["variants"]:
        assert v["size"] <= MAX_FILE, (it["id"], v["name"], v["size"])
    return rec, orig


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--stage", required=True)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--only", default="")
    ap.add_argument("--jobs", type=int, default=3)
    a = ap.parse_args()
    srcf = os.path.join(REPO, "tools/studio/bg-sources.json")
    catf = os.path.join(REPO, "assets/studio/backgrounds.json")
    src = json.load(open(srcf, encoding="utf-8"))
    if a.only.startswith("@"):
        a.only = open(a.only[1:]).read().strip()
    only = set(filter(None, a.only.split(",")))
    os.makedirs(os.path.join(a.stage, "videos"), exist_ok=True)
    thumbs = os.path.join(REPO, "assets/studio/backgrounds")
    todo = []
    for it in src["items"]:
        assert it["category"] in CATEGORIES, it["id"]
        if it["kind"] != "video" or (only and it["id"] not in only):
            continue
        todo.append(it)
    results = {}
    with ProcessPoolExecutor(max_workers=a.jobs) as ex:
        for it, (rec, orig) in zip(todo, ex.map(build_one, [(it, a.src, a.stage, a.force) for it in todo])):
            results[it["id"]] = (rec, orig)
            print("built", it["id"], [v["size"] for v in rec["variants"]], flush=True)
    # the record is read again, so builds of different items (--only) may run side by side
    fresh = json.load(open(srcf, encoding="utf-8"))
    for i in fresh["items"]:
        if i["id"] in results and results[i["id"]][1]:
            i["origin"]["original"] = results[i["id"]][1]
    with open(srcf + ".part", "w", encoding="utf-8") as f:
        json.dump(fresh, f, indent=1, ensure_ascii=False)
        f.write("\n")
    os.replace(srcf + ".part", srcf)
    if only:
        return
    loops = [results[i["id"]][0] for i in fresh["items"] if i["id"] in results]
    for it in fresh["items"]:
        if it["id"] in results:
            it["variants"] = results[it["id"]][0]["variants"]
    # the pictures stay exactly as the catalog has them
    old = json.load(open(catf, encoding="utf-8"))
    pictures = [i for i in old["items"] if i.get("kind") == "picture"]
    ok = {i["id"] + ".thumb.jpg" for i in loops}
    for f in glob.glob(os.path.join(thumbs, "loop-*.thumb.jpg")):
        if os.path.basename(f) not in ok:
            os.remove(f)
    keep = {v["file"] for i in loops for v in i["variants"]}
    for f in glob.glob(os.path.join(a.stage, "videos", "*.mp4")):
        if os.path.relpath(f, a.stage) not in keep:
            os.remove(f)
    cat = {"format": 1, "base": BASE, "items": pictures + loops}
    with open(catf, "w", encoding="utf-8") as f:
        json.dump(cat, f, indent=1, ensure_ascii=False)
        f.write("\n")
    vids = [i for i in fresh["items"] if i["id"] in results]
    readme(vids, a.stage)
    manifest(loops, a.stage)
    print(len(pictures), "pictures kept,", len(loops), "loops")


if __name__ == "__main__":
    main()
