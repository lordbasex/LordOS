#!/usr/bin/env python3
"""Procedural backgrounds for LordOS Studio (license MIT, author LordOS).

Renders slow, low-contrast animated loops and a few stills that sit behind a person in a video call.
Every loop is periodic by construction (all motion is a function of sin/cos of whole multiples of the
loop phase), so the last frame flows into the first without a crossfade. Output is deterministic: each
item has its own fixed seed.

usage: python3 -I tools/studio/gen-studio-loops.py <outdir> [id ...]
  loops  -> <outdir>/<id>.mkv  (FFV1 lossless, 1920x1080, 30 fps, 8 s; tools/studio/build-backgrounds.py encodes it)
  stills -> <outdir>/<id>.png  (1920x1080)
"""
import math
import os
import subprocess
import sys

import numpy as np

W, H = 1920, 1080
LW, LH = 480, 270          # low-resolution grid for smooth fields
FPS, SECONDS = 30, 8
NFRAMES = FPS * SECONDS
TAU = 2 * math.pi


def hexrgb(h):
    h = h.lstrip("#")
    return np.array([int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)], np.float32)


def srgb_to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lin_to_srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def upscale(field):
    """Smooth low-res (LH, LW, C) float field -> (H, W, C), bilinear."""
    from PIL import Image
    return np.stack([np.asarray(Image.fromarray(np.ascontiguousarray(field[..., c], np.float32), "F").resize((W, H), Image.BILINEAR))
                     for c in range(field.shape[2])], -1)


LY, LX = np.mgrid[0:LH, 0:LW].astype(np.float32)
LX = (LX + 0.5) / LW * (W / H)     # x in [0, 16/9]
LY = (LY + 0.5) / LH                # y in [0, 1]
FY, FX = np.mgrid[0:H, 0:W].astype(np.float32)


def vignette(strength=0.35, power=2.2):
    cx, cy = W / 2, H / 2
    r = np.sqrt(((FX - cx) / (W * 0.62)) ** 2 + ((FY - cy) / (H * 0.62)) ** 2)
    return (1 - strength * np.clip(r, 0, 1.6) ** power)[..., None].astype(np.float32)


VIG = vignette()


LUT_N = 16384
SRGB_LUT = (lin_to_srgb(np.arange(LUT_N, dtype=np.float64) / (LUT_N - 1)) * 255.0).astype(np.float32)
_DITHER = {}


def finish(lin, frame_seed):
    """Linear RGB float (H, W, 3) -> sRGB uint8 with a light triangular dither (fixed seeded patterns)."""
    idx = np.clip(lin * (LUT_N - 1) + 0.5, 0, LUT_N - 1).astype(np.int32)
    s = SRGB_LUT[idx]
    k = frame_seed % 8
    if k not in _DITHER:
        rng = np.random.default_rng(1000 + k)
        _DITHER[k] = (rng.random((H, W, 1), np.float32) - rng.random((H, W, 1), np.float32) + 0.5)
    s += _DITHER[k]
    return np.clip(s, 0, 255).astype(np.uint8)


# ---------------------------------------------------------------- scenes

def blobs_scene(palette, bg_top, bg_bottom, n=6, size=(0.35, 0.65), amp=0.18, seed=1):
    rng = np.random.default_rng(seed)
    cols = [srgb_to_lin(hexrgb(c)) for c in palette]
    bl = []
    for i in range(n):
        bl.append(dict(cx=rng.uniform(0.1, 1.68), cy=rng.uniform(0.1, 0.9), r=rng.uniform(*size),
                       ax=rng.uniform(0.5, 1.0) * amp, ay=rng.uniform(0.5, 1.0) * amp,
                       kx=int(rng.integers(1, 3)), ky=int(rng.integers(1, 3)),
                       px=rng.uniform(0, TAU), py=rng.uniform(0, TAU), col=cols[i % len(cols)],
                       w=rng.uniform(0.6, 1.0)))
    top, bot = srgb_to_lin(hexrgb(bg_top)), srgb_to_lin(hexrgb(bg_bottom))
    base = top[None, None, :] * (1 - LY[..., None]) + bot[None, None, :] * LY[..., None]

    def frame(t):
        th = TAU * t
        acc = np.zeros((LH, LW, 3), np.float32)
        wsum = np.full((LH, LW, 1), 0.35, np.float32)
        for b in bl:
            x = b["cx"] + b["ax"] * math.sin(b["kx"] * th + b["px"])
            y = b["cy"] + b["ay"] * math.cos(b["ky"] * th + b["py"])
            g = np.exp(-((LX - x) ** 2 + (LY - y) ** 2) / (b["r"] ** 2))[..., None] * b["w"]
            acc += g * b["col"]
            wsum += g
        field = (acc + 0.35 * base) / wsum
        return upscale(field) * VIG
    return frame


def bokeh_scene(palette, bg_top, bg_bottom, n=36, rmin=18, rmax=95, amp=60, glow=0.4, seed=2):
    rng = np.random.default_rng(seed)
    cols = [srgb_to_lin(hexrgb(c)) for c in palette]
    top, bot = srgb_to_lin(hexrgb(bg_top)), srgb_to_lin(hexrgb(bg_bottom))
    bg = upscale(top[None, None, :] * (1 - LY[..., None]) + bot[None, None, :] * LY[..., None]) * VIG
    lights = []
    for i in range(n):
        r = float(rng.uniform(rmin, rmax))
        R = int(math.ceil(r + 3))
        yy, xx = np.mgrid[-R:R + 1, -R:R + 1].astype(np.float32)
        d = np.sqrt(xx ** 2 + yy ** 2)
        disc = np.clip((r - d) / 2.5 + 0.5, 0, 1)                    # anti-aliased disc
        rim = np.exp(-((d - r * 0.92) / (r * 0.08 + 1)) ** 2) * 0.35  # soft brighter rim
        sprite = (disc * (0.65 + 0.35 * (d / r) ** 2) + rim * disc)[..., None]
        lights.append(dict(sprite=sprite.astype(np.float32), R=R, cx=rng.uniform(0, W), cy=rng.uniform(0.15, 1.0) * H,
                           ax=rng.uniform(0.4, 1.0) * amp, ay=rng.uniform(0.3, 0.8) * amp,
                           kx=int(rng.integers(1, 3)), ky=int(rng.integers(1, 3)),
                           px=rng.uniform(0, TAU), py=rng.uniform(0, TAU), pb=rng.uniform(0, TAU),
                           col=cols[int(rng.integers(0, len(cols)))] * rng.uniform(0.25, 0.75) * glow * (40 / r) ** 0.5))

    def frame(t):
        th = TAU * t
        img = bg.copy()
        for L in lights:
            x = int(round(L["cx"] + L["ax"] * math.sin(L["kx"] * th + L["px"])))
            y = int(round(L["cy"] + L["ay"] * math.cos(L["ky"] * th + L["py"])))
            k = 0.75 + 0.25 * math.sin(th + L["pb"])
            R = L["R"]
            x0, y0, x1, y1 = x - R, y - R, x + R + 1, y + R + 1
            sx0, sy0 = max(0, -x0), max(0, -y0)
            x0c, y0c, x1c, y1c = max(x0, 0), max(y0, 0), min(x1, W), min(y1, H)
            if x1c <= x0c or y1c <= y0c:
                continue
            sp = L["sprite"][sy0:sy0 + (y1c - y0c), sx0:sx0 + (x1c - x0c)]
            img[y0c:y1c, x0c:x1c] += sp * (L["col"] * k)
        return img
    return frame


def stars(n, seed, bright=1.0):
    rng = np.random.default_rng(seed)
    return dict(x=rng.integers(0, W, n), y=rng.integers(0, H, n), b=(rng.random(n) ** 3 * 0.5 + 0.03) * bright,
                ph=rng.uniform(0, TAU, n), k=rng.integers(1, 4, n))


def draw_stars(img, st, th):
    b = st["b"] * (0.7 + 0.3 * np.sin(st["k"] * th + st["ph"]))
    for dy, dx, w in ((0, 0, 1.0), (0, 1, 0.3), (0, -1, 0.3), (1, 0, 0.3), (-1, 0, 0.3)):
        yy = np.clip(st["y"] + dy, 0, H - 1)
        xx = np.clip(st["x"] + dx, 0, W - 1)
        np.add.at(img, (yy, xx), (b * w)[:, None] * np.array([0.9, 0.95, 1.0], np.float32))


def aurora_scene(c_low, c_high, sky_top, sky_bottom, seed=3, horizon=True):
    rng = np.random.default_rng(seed)
    lo, hi = srgb_to_lin(hexrgb(c_low)), srgb_to_lin(hexrgb(c_high))
    top, bot = srgb_to_lin(hexrgb(sky_top)), srgb_to_lin(hexrgb(sky_bottom))
    sky = upscale(top[None, None, :] * (1 - LY[..., None]) + bot[None, None, :] * LY[..., None])
    st = stars(900, seed + 100)
    waves = [(int(rng.integers(1, 4)), rng.uniform(1.5, 6.0), rng.uniform(0, TAU), rng.uniform(0.01, 0.05)) for _ in range(6)]
    stripes = [(rng.uniform(10, 70), int(rng.integers(1, 3)) * (1 if rng.random() < 0.5 else -1), rng.uniform(0, TAU), rng.uniform(0.04, 0.12)) for _ in range(7)]
    ridge = 0.86 + 0.03 * np.sin(LX[0] * 3.1 + 1.0) + 0.02 * np.sin(LX[0] * 7.3 + 2.0)

    def frame(t):
        th = TAU * t
        x = LX[0]
        base = 0.5 + sum(a * np.sin(f * x + k * th + p) for k, f, p, a in waves)
        s = 0.75 + sum(a * np.sin(f * x + k * th + p) for f, k, p, a in stripes)
        s = np.clip(s, 0.2, 1.4) * (0.75 + 0.25 * np.sin(1.3 * x + th + seed))
        d = LY - base[None, :]
        curtain = np.where(d < 0, np.exp(-(d / 0.26) ** 2), np.exp(-(d / 0.04) ** 2)) * s[None, :]
        mix = np.clip(-d / 0.3, 0, 1)[..., None]
        col = lo * (1 - mix) + hi * mix
        field = curtain[..., None] * col * 0.45
        img = sky + upscale(field)
        draw_stars(img, st, th)
        if horizon:
            hz = upscale((LY > ridge[None, :]).astype(np.float32)[..., None])
            img = img * (1 - hz) + hz * srgb_to_lin(hexrgb("#05070a"))
        return img * VIG
    return frame


def waves_scene(cols, sky_top, sky_bottom, seed=4):
    rng = np.random.default_rng(seed)
    cs = [srgb_to_lin(hexrgb(c)) for c in cols]
    top, bot = srgb_to_lin(hexrgb(sky_top)), srgb_to_lin(hexrgb(sky_bottom))
    sky = top[None, None, :] * (1 - LY[..., None]) + bot[None, None, :] * LY[..., None]
    layers = []
    n = len(cs)
    for i in range(n):
        layers.append(dict(base=0.42 + 0.5 * i / n, comps=[(rng.uniform(1.0, 4.0), int(rng.integers(1, 3)) * (1 if rng.random() < 0.5 else -1),
                                                          rng.uniform(0, TAU), rng.uniform(0.008, 0.03)) for _ in range(3)], col=cs[i]))

    def frame(t):
        th = TAU * t
        field = sky.copy()
        x = LX[0]
        for L in layers:
            yb = L["base"] + sum(a * np.sin(f * x + k * th + p) for f, k, p, a in L["comps"])
            m = np.clip((LY - yb[None, :]) / 0.012 + 0.5, 0, 1)[..., None]
            shade = 1.0 - 0.25 * np.clip((LY - yb[None, :]) / 0.25, 0, 1)[..., None]
            field = field * (1 - m) + m * (L["col"] * shade)
        return upscale(field) * VIG
    return frame


def particles_scene(bg_top, bg_bottom, tint, n=420, seed=5):
    rng = np.random.default_rng(seed)
    top, bot = srgb_to_lin(hexrgb(bg_top)), srgb_to_lin(hexrgb(bg_bottom))
    bg = upscale(top[None, None, :] * (1 - LY[..., None]) + bot[None, None, :] * LY[..., None]) * VIG
    col = srgb_to_lin(hexrgb(tint))
    sprites = {}
    for r in (1.2, 1.8, 2.6, 3.6, 5.0):
        R = int(math.ceil(r * 2.5))
        yy, xx = np.mgrid[-R:R + 1, -R:R + 1].astype(np.float32)
        sprites[r] = (np.exp(-(xx ** 2 + yy ** 2) / (r * r)))[..., None].astype(np.float32)
    P = [dict(r=float(rng.choice(list(sprites))), cx=rng.uniform(0, W), cy=rng.uniform(0, H),
              ax=rng.uniform(20, 90), ay=rng.uniform(20, 90), kx=int(rng.integers(1, 3)), ky=int(rng.integers(1, 3)),
              px=rng.uniform(0, TAU), py=rng.uniform(0, TAU), b=rng.uniform(0.08, 0.5), pb=rng.uniform(0, TAU)) for _ in range(n)]

    def frame(t):
        th = TAU * t
        img = bg.copy()
        for p in P:
            sp = sprites[p["r"]]
            R = sp.shape[0] // 2
            x = int(round(p["cx"] + p["ax"] * math.sin(p["kx"] * th + p["px"])))
            y = int(round(p["cy"] + p["ay"] * math.cos(p["ky"] * th + p["py"])))
            if x - R < 0 or y - R < 0 or x + R + 1 > W or y + R + 1 > H:
                continue
            img[y - R:y + R + 1, x - R:x + R + 1] += sp * col * (p["b"] * (0.6 + 0.4 * math.sin(th + p["pb"])))
        return img
    return frame


def backdrop_scene(c_center, c_edge, amp=0.06, seed=6, spot=0.75):
    """Seamless photo-studio paper backdrop with a slowly wandering soft key light."""
    rng = np.random.default_rng(seed)
    ce, cd = srgb_to_lin(hexrgb(c_center)), srgb_to_lin(hexrgb(c_edge))
    ph = rng.uniform(0, TAU, 2)
    floor = np.clip((LY - 0.72) / 0.28, 0, 1)[..., None]   # paper sweep: slightly darker floor

    def frame(t):
        th = TAU * t
        x = 0.89 + amp * math.sin(th + ph[0])
        y = 0.42 + amp * 0.6 * math.sin(2 * th + ph[1])
        r2 = ((LX - x) / 0.95) ** 2 + ((LY - y) / 0.75) ** 2
        k = np.exp(-r2 * 1.6)[..., None] * spot + (1 - spot) * 0.6
        field = cd * (1 - k) + ce * k
        field = field * (1 - 0.12 * floor)
        return upscale(field) * vignette(0.28, 2.0)
    return frame


def starfield_scene(seed=7):
    rng = np.random.default_rng(seed)
    st = stars(2600, seed + 1, 1.2)
    neb = blobs_scene(["#3a2c6e", "#1f5578", "#552452", "#16405a"], "#04050a", "#070a12", n=7, size=(0.25, 0.5), amp=0.05, seed=seed + 2)

    def frame(t):
        img = neb(t) * 1.1
        draw_stars(img, st, TAU * t)
        return img
    return frame


LOOPS = {
    # abstract: soft gradient flows
    "flow-ocean":       lambda: blobs_scene(["#1d4e89", "#2a7f9e", "#16324f", "#3c6e91"], "#0e2236", "#123049", seed=11),
    "flow-peach":       lambda: blobs_scene(["#e9b89a", "#d98f7c", "#f3d1b5", "#c97f8f"], "#e4c2ad", "#cf9f93", seed=12),
    "flow-violet":      lambda: blobs_scene(["#4b3b8f", "#7a4c9e", "#2e2a6b", "#a16aa8"], "#1f1a42", "#2a2152", seed=13),
    "flow-mint":        lambda: blobs_scene(["#9fd8c4", "#6fb7a8", "#cfe8d8", "#7fa9b8"], "#b9ddd0", "#8fbfb3", seed=14),
    "flow-graphite":    lambda: blobs_scene(["#4a4f57", "#5d636d", "#3a3e45", "#6b6f77"], "#2b2e33", "#34373d", seed=15),
    # bokeh lights
    "bokeh-amber":      lambda: bokeh_scene(["#ffb35c", "#ffd08a", "#ff8f4a", "#ffe2b0"], "#1a1210", "#2a1a14", seed=21),
    "bokeh-blue":       lambda: bokeh_scene(["#6fb6ff", "#9fd0ff", "#4a7dff", "#b7a6ff"], "#0b1020", "#121a2e", seed=22),
    "bokeh-city-night": lambda: bokeh_scene(["#ffcf7a", "#ff7a59", "#7ac8ff", "#fff1c9", "#ff5f8a"], "#07090f", "#151018",
                                            n=70, rmin=14, rmax=70, amp=35, seed=23),
    # sky
    "aurora-green":     lambda: aurora_scene("#38ff9c", "#3a7bff", "#02040a", "#0a1626", seed=31),
    "aurora-violet":    lambda: aurora_scene("#b06bff", "#ff5fa8", "#03030a", "#140f26", seed=32),
    "starfield-nebula": lambda: starfield_scene(seed=37),
    # waves and particles
    "waves-dusk":       lambda: waves_scene(["#6c5b8f", "#4f4a7d", "#363562", "#24264a"], "#f0b38f", "#9f7aa0", seed=41),
    "waves-mint":       lambda: waves_scene(["#8cc7b5", "#5fa59a", "#3f8080", "#28605f"], "#e8f1ea", "#bcdcd0", seed=42),
    "particles-dust":   lambda: particles_scene("#14161c", "#22252d", "#ffe7c2", seed=51),
    # studio backdrops
    "backdrop-grey":    lambda: backdrop_scene("#9da2a8", "#3d4045", seed=61),
    "backdrop-teal":    lambda: backdrop_scene("#4f8a8f", "#16292d", seed=62),
}

STILLS = {
    "backdrop-warm-still": lambda: backdrop_scene("#c9b49c", "#5a4a3d", amp=0.0, seed=71),
    "backdrop-navy-still": lambda: backdrop_scene("#3f5b86", "#111a2b", amp=0.0, seed=72),
    "gradient-dawn-still": lambda: blobs_scene(["#f2c6a0", "#c9a4c9", "#8fb0d8", "#f5dcc0"], "#d8c0c8", "#a8b8d8", seed=73),
}


def render_loop(name, out):
    fn = LOOPS[name]()
    tmp = out + ".part.mkv"
    p = subprocess.Popen(["ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
                          "-r", str(FPS), "-i", "-", "-c:v", "ffv1", "-level", "3", "-pix_fmt", "bgr0", "-fflags", "+bitexact",
                          "-map_metadata", "-1", tmp], stdin=subprocess.PIPE)
    seed = sum(name.encode())
    for i in range(NFRAMES):
        p.stdin.write(finish(fn(i / NFRAMES), seed * 100000 + i).tobytes())
    p.stdin.close()
    if p.wait() != 0:
        raise SystemExit(f"ffmpeg failed for {name}")
    os.replace(tmp, out)


def render_still(name, out):
    from PIL import Image
    fn = STILLS[name]()
    Image.fromarray(finish(fn(0.0), sum(name.encode()))).save(out)


def main():
    outd = sys.argv[1]
    os.makedirs(outd, exist_ok=True)
    names = sys.argv[2:] or list(LOOPS) + list(STILLS)
    for n in names:
        if n in LOOPS:
            render_loop(n, os.path.join(outd, n + ".mkv"))
        elif n in STILLS:
            render_still(n, os.path.join(outd, n + ".png"))
        else:
            raise SystemExit("unknown id " + n)
        print("rendered", n, flush=True)


if __name__ == "__main__":
    main()
