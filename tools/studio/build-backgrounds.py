#!/usr/bin/env python3
"""Build the LordOS Studio background catalog's loops from tools/studio/bg-sources.json.

Since 2026-11-22 the pictures are AI-generated and made by tools/studio/build-ai-backgrounds.py; this script keeps the
catalog's picture items as they are and rebuilds the loops (kind video).

Inputs are the downloads kept outside the repository (one folder per source):
  <src>/polyhaven/jpg/<asset>.jpg      Poly Haven tonemapped equirectangular panoramas
  <src>/ambientcg/<asset>_Color.jpg    ambientCG colour maps
  <src>/nasa/<nasa_id>.jpg             NASA originals
  <src>/commons/full*/<key>.<ext>      Wikimedia Commons originals (key = sha1(title)[:12])
  <src>/procedural/<item>.mkv|.png     renders of tools/studio/gen-studio-loops.py

Outputs:
  <stage>/pictures/<id>.1080.jpg, <stage>/videos/<id>.{720,1080}.h264.mp4, <stage>/README.md
  assets/studio/backgrounds/<id>.thumb.jpg, assets/studio/backgrounds/<id>.jpg (built-ins),
  assets/studio/backgrounds.json

The downloads are untrusted data: run with `python3 -I`, never from inside the download folders.

usage: python3 -I tools/studio/build-backgrounds.py --src DIR --stage DIR [--force] [--only id,id]
"""
import argparse
import glob
import hashlib
import io
import json
import math
import os
import subprocess
import sys

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASE = "https://raw.githubusercontent.com/lordbasex/LordOS-wallpapers/main/studio"
OW, OH = 1920, 1080
JPEG_Q = 88
THUMB_Q = 80
CATEGORIES = ["office", "home", "studio", "nature", "city", "abstract", "space"]
# One built-in still per category ships in the package so the feature works offline.
BUILTIN = {"office-reading-room", "home-sunny-lounge", "studio-backdrop-warm", "nature-misty-pines",
           "city-lagoon-dawn", "abstract-dawn-gradient", "space-orbital-sunset"}


def save_jpeg(img, path, q):
    tmp = path + ".part"
    img.convert("RGB").save(tmp, "JPEG", quality=q, optimize=True, progressive=False, subsampling="4:2:0")
    os.replace(tmp, path)


# ------------------------------------------------------------------ pictures

def equirect_view(path, yaw, pitch, fov, w=OW, h=OH):
    im = Image.open(path)
    im = im.convert("RGB")
    # Pre-filter the panorama to about one source pixel per output pixel, then sample bilinearly.
    target_w = int(round(w * 360.0 / fov))
    if im.width > target_w:
        im = im.resize((target_w, target_w // 2), Image.LANCZOS)
    eq = np.asarray(im)
    He, We, _ = eq.shape
    f = (w / 2) / math.tan(math.radians(fov) / 2)
    X, Y = np.meshgrid(np.arange(w) - w / 2 + 0.5, np.arange(h) - h / 2 + 0.5)
    d = np.stack([X, -Y, np.full_like(X, f)], -1)
    d /= np.linalg.norm(d, axis=-1, keepdims=True)
    p, yw = math.radians(pitch), math.radians(yaw)
    rx = np.array([[1, 0, 0], [0, math.cos(p), math.sin(p)], [0, -math.sin(p), math.cos(p)]])
    ry = np.array([[math.cos(yw), 0, math.sin(yw)], [0, 1, 0], [-math.sin(yw), 0, math.cos(yw)]])
    d = d @ rx.T @ ry.T
    lon = np.arctan2(d[..., 0], d[..., 2])
    lat = np.arcsin(np.clip(d[..., 1], -1, 1))
    u = (lon / (2 * math.pi) + 0.5) * We - 0.5
    v = (0.5 - lat / math.pi) * He - 0.5
    u0 = np.floor(u).astype(np.int64)
    v0 = np.floor(v).astype(np.int64)
    fu = (u - u0)[..., None].astype(np.float32)
    fv = (v - v0)[..., None].astype(np.float32)
    u0m, u1m = u0 % We, (u0 + 1) % We
    v0c, v1c = np.clip(v0, 0, He - 1), np.clip(v0 + 1, 0, He - 1)
    out = (eq[v0c, u0m] * ((1 - fu) * (1 - fv)) + eq[v0c, u1m] * (fu * (1 - fv)) +
           eq[v1c, u0m] * ((1 - fu) * fv) + eq[v1c, u1m] * (fu * fv))
    return Image.fromarray(np.clip(out + 0.5, 0, 255).astype(np.uint8))


def crop_fraction(path, cx, cy, cw):
    im = Image.open(path).convert("RGB")
    w = int(round(im.width * cw))
    h = int(round(w * 9 / 16))
    if h > im.height:
        h = im.height
        w = int(round(h * 16 / 9))
    x = int(round(im.width * cx))
    x = min(max(0, x), im.width - w)
    if cy is None:
        y = im.height - h
    else:
        y = int(round(im.height * cy)) if cy != 0.5 else (im.height - h) // 2
        y = min(max(0, y), im.height - h)
    im = im.crop((x, y, x + w, y + h))
    if im.width < OW:
        raise SystemExit(f"{path}: crop {im.size} would need upscaling")
    return im.resize((OW, OH), Image.LANCZOS)


def texture_wall(path):
    im = Image.open(path).convert("RGB")
    s = min(im.width, im.height * 16 // 9)
    h = s * 9 // 16
    x, y = (im.width - s) // 2, (im.height - h) // 2
    im = im.crop((x, y, x + s, y + h)).resize((OW, OH), Image.LANCZOS)
    a = np.asarray(im).astype(np.float32) / 255.0
    lin = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    yy, xx = np.mgrid[0:OH, 0:OW].astype(np.float32)
    key = np.exp(-(((xx - OW * 0.45) / (OW * 0.55)) ** 2 + ((yy - OH * 0.35) / (OH * 0.75)) ** 2))
    r = np.sqrt(((xx - OW / 2) / (OW * 0.62)) ** 2 + ((yy - OH / 2) / (OH * 0.62)) ** 2)
    light = (0.55 + 0.45 * key) * (1 - 0.3 * np.clip(r, 0, 1.6) ** 2)
    lin = lin * light[..., None]
    out = np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.power(np.clip(lin, 0, 1), 1 / 2.4) - 0.055)
    return Image.fromarray(np.clip(out * 255 + 0.5, 0, 255).astype(np.uint8))


def build_picture(it, src, stage, force):
    out = os.path.join(stage, "pictures", it["id"] + ".1080.jpg")
    if os.path.exists(out) and not force:
        return out
    o, p = it["origin"], it["process"]
    if o["type"] == "polyhaven":
        img = equirect_view(os.path.join(src, "polyhaven/jpg", o["asset"] + ".jpg"), p["yaw"], p["pitch"], p["fov"])
    elif o["type"] == "ambientcg":
        img = texture_wall(os.path.join(src, "ambientcg", o["asset"] + "_Color.jpg"))
    elif o["type"] == "nasa":
        c = p["crop"]
        img = crop_fraction(os.path.join(src, "nasa", o["asset"] + ".jpg"), c["x"], c["y"], c["w"])
    elif o["type"] == "procedural":
        img = Image.open(os.path.join(src, "procedural", o["item"] + ".png")).convert("RGB")
    else:
        raise SystemExit("unknown origin " + o["type"])
    assert img.size == (OW, OH), (it["id"], img.size)
    save_jpeg(img, out, JPEG_Q)
    return out


# ------------------------------------------------------------------ videos

def probe(path):
    j = json.loads(subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                   "stream=width,height,r_frame_rate", "-of", "json", path],
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


X264 = ["-c:v", "libx264", "-profile:v", "high", "-preset", "slow", "-pix_fmt", "yuv420p", "-an", "-sn", "-dn",
        "-map_metadata", "-1", "-map_chapters", "-1", "-movflags", "+faststart", "-fflags", "+bitexact",
        "-flags:v", "+bitexact", "-x264-params", "threads=8"]
RATE = {720: ["-crf", "21", "-maxrate", "3200k", "-bufsize", "6400k"],
        1080: ["-crf", "21", "-maxrate", "6000k", "-bufsize", "12000k"]}


def build_video(it, src, stage, force):
    o, p = it["origin"], it["process"]
    if o["type"] == "procedural":
        path = os.path.join(src, "procedural", o["item"] + ".mkv")
    else:
        cands = glob.glob(os.path.join(src, "commons", "full*", o["localKey"] + ".*"))
        cands = [c for c in cands if not c.endswith(".part")]
        if len(cands) != 1:
            raise SystemExit(f"{it['id']}: source file not found for {o['localKey']}")
        path = cands[0]
    w, h, n, d = probe(path)
    crop = p.get("crop")
    if crop:
        cx, cy, cw, ch = crop
        w, h = cw, ch
    on, od = out_rate(n, d)
    secs = p["seconds"]
    sizes = [720] + ([1080] if h >= 1080 and w >= 1920 else [])
    outs = {s: os.path.join(stage, "videos", f"{it['id']}.{s}.h264.mp4") for s in sizes}
    if all(os.path.exists(f) for f in outs.values()) and not force:
        return outs, on / od, secs
    pre = (f"crop={cw}:{ch}:{cx}:{cy}," if crop else "")
    top = sizes[-1]
    tw, th = (1920, 1080) if top == 1080 else (1280, 720)
    pre += f"scale={tw}:{th}:flags=lanczos,fps={on}/{od},setsar=1,format=yuv444p"
    if o["type"] == "procedural":
        graph = f"[0:v]{pre},trim=duration={secs},setpts=PTS-STARTPTS[o]"
        inp = ["-i", path]
    else:
        # The clip [start, start+secs] is closed by crossfading the second after its end over its first second,
        # so the last frame flows into the first.
        graph = (f"[0:v]{pre},setpts=PTS-STARTPTS,split[a][b];"
                 f"[a]trim=start=0:end={secs},setpts=PTS-STARTPTS,fps={on}/{od}[A];"
                 f"[b]trim=start={secs}:end={secs + 1},setpts=PTS-STARTPTS,fps={on}/{od}[B];"
                 f"[B][A]xfade=transition=fade:duration=1:offset=0[o]")
        inp = ["-ss", str(p["start"]), "-t", str(secs + 1.2), "-i", path]
    if len(sizes) == 2:
        graph += ";[o]split[o1][o2];[o2]scale=1280:720:flags=lanczos[o720]"
        maps = {1080: "[o1]", 720: "[o720]"}
    else:
        maps = {720: "[o]"}
    cmd = ["ffmpeg", "-nostdin", "-v", "error", "-y"] + inp + ["-filter_complex", graph]
    for s in sizes:
        cmd += ["-map", maps[s], "-t", str(secs), "-r", f"{on}/{od}", "-g", str(int(round(2 * on / od)))] + X264 + RATE[s] + [outs[s] + ".part.mp4"]
    subprocess.run(cmd, check=True)
    for s in sizes:
        os.replace(outs[s] + ".part.mp4", outs[s])
    return outs, on / od, secs


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


def readme(items, stage):
    L = ["# LordOS Studio backgrounds", "",
         "Backgrounds for the LordOS Studio camera effects (background replacement). LordOS downloads these files on",
         "demand; the catalog is `assets/studio/backgrounds.json` in the LordOS repository, and",
         "`tools/studio/bg-sources.json` records where each file came from and how it was made.", "",
         "Every file keeps the license of its source. Pictures are 1920x1080 JPEG; loops are H.264 MP4 without audio,",
         "closed into a seamless loop (the second after the clip's end is crossfaded over its first second).",
         "Our own renders (author LordOS) are MIT licensed and made by `tools/studio/gen-studio-loops.py`.", "",
         "| File | Title | Author | License | Source | What we did |", "|---|---|---|---|---|---|"]
    for it in items:
        p, o = it["process"], it["origin"]
        if p["op"] == "equirect-to-perspective":
            did = f"Rendered a perspective view from the panorama (yaw {p['yaw']}, pitch {p['pitch']}, FOV {p['fov']} deg)"
        elif p["op"] == "texture-crop":
            did = "Cropped the colour map to 16:9, resized, added a soft light falloff"
        elif p["op"] == "crop":
            did = "Cropped to 16:9 (station hardware left out), resized"
        elif p["op"] == "render":
            did = "Procedural render"
        elif p["op"] == "encode":
            did = f"Procedural render, {p['seconds']} s loop"
        else:
            did = f"Trimmed {p['start']}-{p['start'] + p['seconds']} s, crossfaded into a loop, scaled"
            if p.get("crop"):
                did += ", cropped to 16:9"
            did += ", audio removed"
        lic = f"[{it['license']}]({it['licenseUrl']})" if it["licenseUrl"] else it["license"]
        files = ", ".join(f"`{v['file']}`" for v in it["variants"])
        L.append(f"| {files} | {it['name']} | {it['author']} | {lic} | {it['source']} | {did} |")
    L += ["", "Poly Haven assets: CC0 (https://polyhaven.com/license). ambientCG assets: CC0 (https://docs.ambientcg.com/license/).",
          "NASA images and video: public domain in the United States (NASA media usage guidelines); NASA does not endorse LordOS.",
          "Wikimedia Commons files: license as stated on each file page; CC BY and CC BY-SA files require the attribution shown",
          "in the catalog's `credit` field. CC BY-SA files stay under CC BY-SA.", ""]
    with open(os.path.join(stage, "README.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(L))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--stage", required=True)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    src = json.load(open(os.path.join(REPO, "tools/studio/bg-sources.json"), encoding="utf-8"))
    if a.only.startswith("@"):
        a.only = open(a.only[1:]).read().strip()
    only = set(filter(None, a.only.split(",")))
    for d in ("pictures", "videos"):
        os.makedirs(os.path.join(a.stage, d), exist_ok=True)
    thumbs = os.path.join(REPO, "assets/studio/backgrounds")
    os.makedirs(thumbs, exist_ok=True)
    out_items = []
    for it in src["items"]:
        if only and it["id"] not in only:
            continue
        assert it["category"] in CATEGORIES, it["id"]
        rec = {k: it[k] for k in ("id", "kind", "category", "name", "author", "license", "licenseUrl", "source")}
        if "credit" in it:
            rec["credit"] = it["credit"]
        if it["kind"] == "picture":
            f = build_picture(it, a.src, a.stage, a.force)
            rec["variants"] = [{"name": "1080", "w": OW, "h": OH, "file": f"pictures/{it['id']}.1080.jpg",
                                "size": os.path.getsize(f), "sha256": sha256(f)}]
            thumb_src = Image.open(f).convert("RGB")
            if it["id"] in BUILTIN:
                rec["builtin"] = True
                with open(f, "rb") as fi, open(os.path.join(thumbs, it["id"] + ".jpg"), "wb") as fo:
                    fo.write(fi.read())
        else:
            outs, fps, secs = build_video(it, a.src, a.stage, a.force)
            rec["variants"] = []
            for s in sorted(outs):
                w, h = (1280, 720) if s == 720 else (1920, 1080)
                rec["variants"].append({"name": f"{s}.h264", "codec": "h264", "w": w, "h": h, "fps": round(fps, 3), "seconds": secs,
                                        "file": f"videos/{it['id']}.{s}.h264.mp4", "size": os.path.getsize(outs[s]), "sha256": sha256(outs[s])})
            thumb_src = frame_at(outs[max(outs)], secs * 0.5)
        save_jpeg(thumb_src.resize((320, 180), Image.LANCZOS), os.path.join(thumbs, it["id"] + ".thumb.jpg"), THUMB_Q)
        it["variants"] = rec["variants"]
        out_items.append(rec)
        print("built", it["id"], [v["size"] for v in rec["variants"]], flush=True)
    if only:
        return
    # the pictures are AI-generated since 2026-11-22 (tools/studio/build-ai-backgrounds.py): keep the catalog's
    cur = json.load(open(os.path.join(REPO, "assets/studio/backgrounds.json"), encoding="utf-8"))
    out_items = [i for i in cur["items"] if i["kind"] == "picture"] + [i for i in out_items if i["kind"] != "picture"]
    for old in glob.glob(os.path.join(thumbs, "*.jpg")):
        base = os.path.basename(old)
        ok = {i["id"] + ".thumb.jpg" for i in out_items} | {i["id"] + ".jpg" for i in out_items if i.get("builtin")}
        if base not in ok:
            os.remove(old)
    cat = {"format": 1, "base": BASE, "items": out_items}
    with open(os.path.join(REPO, "assets/studio/backgrounds.json"), "w", encoding="utf-8") as f:
        json.dump(cat, f, indent=1, ensure_ascii=False)
        f.write("\n")
    readme(src["items"], a.stage)
    print(len(out_items), "items")


if __name__ == "__main__":
    main()
