#!/usr/bin/env python3
"""Find where a clip loops best (LordOS Studio's background loops; tools/studio/build-backgrounds.py).

A loop [start, start+secs] is closed by crossfading the xf seconds after its end over its first xf seconds, so during
the fade two moments `secs` apart are shown on top of each other. The best start is where those two look most alike:
this decodes the clip tiny and grey (32x18: only the broad picture, so waves, snow or flames that differ in detail but
not in shape don't count) and, for every start on a quarter-second grid, measures the mean difference in levels (0-255)
between the frames at t and t+secs over the fade. Under about 3 levels no one sees the seam; above about 6 the fade
shows as a dissolve (a camera that moves, light that changes).

The clip is untrusted data: run with `python3 -I`.

usage: python3 -I tools/studio/pick-loop-start.py FILE [--seconds 7] [--crossfade 1.5] [--max 120] [--at START]
prints: best start, its score, and the score of --at if given
"""
import argparse
import subprocess

import numpy as np

W, H = 32, 18


def frames(path, fps, limit):
    raw = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-t", str(limit), "-i", path, "-an", "-vf",
                          f"fps={fps},scale={W}:{H}:flags=area,format=gray", "-f", "rawvideo", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, H, W).astype(np.float32)


def score(f, fps, start, secs, xf):
    a = int(round(start * fps))
    n = int(round(secs * fps))
    k = max(1, int(round(xf * fps)))
    if a + n + k >= len(f):
        return None
    return float(np.abs(f[a:a + k] - f[a + n:a + n + k]).mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--seconds", type=float, default=7)
    ap.add_argument("--crossfade", type=float, default=1.5)
    ap.add_argument("--max", type=float, default=120, help="look at most this far into the clip (seconds)")
    ap.add_argument("--at", type=float, default=None)
    a = ap.parse_args()
    fps = 12
    f = frames(a.file, fps, a.max)
    if len(f) < 3:
        raise SystemExit("too short")
    base = float(np.median(np.abs(np.diff(f, axis=0)).mean(axis=(1, 2))))
    best = None
    for s in np.arange(0, len(f) / fps, 0.25):
        v = score(f, fps, s, a.seconds, a.crossfade)
        if v is not None and (best is None or v < best[1]):
            best = (float(s), v)
    if best is None:
        raise SystemExit(f"the clip is shorter than {a.seconds + a.crossfade} s")
    out = f"best {best[0]:.2f} score {best[1]:.2f} levels (frame to frame {base:.2f} at {fps} fps)"
    if a.at is not None:
        v = score(f, fps, a.at, a.seconds, a.crossfade)
        out += f"; at {a.at}: {v:.2f}" if v is not None else f"; at {a.at}: too late"
    print(out)


if __name__ == "__main__":
    main()
