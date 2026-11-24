#!/usr/bin/env python3
"""protected_check.py — tests/protected_headless.sh's verdict on the pictures lord-server-test wrote ("frame W", testhooks.h).

  protected_check.py DIR PX,PY,PW,PH OX,OY,OW,OH

DIR holds lordos-test-frame-{output,screenshot,mirror,portal,window-p,window-o}.ppm; P is the protected window's rectangle,
O the other window's (logical px, headless at 100 %). The protected window must be drawn on the output and be the
protected fill (opaque, r = g = b, mostly PAINT_PROTECT_FILL) in every capture; the other window must be captured as
the output shows it. Prints one line per check, exits 1 on a failure."""
import sys

FILL = (0x2E, 0x2E, 0x2E)


def ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    pos += 1
    assert parts[0] == b'P6' and parts[3] == b'255', path
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos:pos + w * h * 3]


def region(img, r, inset=4):
    w, h, px = img
    x, y, rw, rh = r
    out = []
    for yy in range(max(0, y + inset), min(h, y + rh - inset)):
        for xx in range(max(0, x + inset), min(w, x + rw - inset)):
            i = (yy * w + xx) * 3
            out.append(tuple(px[i:i + 3]))
    return out


def main():
    d = sys.argv[1]
    prot = tuple(int(v) for v in sys.argv[2].split(','))
    other = tuple(int(v) for v in sys.argv[3].split(','))
    bad = 0

    def check(ok, what):
        nonlocal bad
        print(('ok   ' if ok else 'FAIL ') + what)
        bad += not ok

    out = ppm(f'{d}/lordos-test-frame-output.ppm')
    po = region(out, prot)
    check(len(po) > 1000, f'the protected window has an area ({len(po)} px)')
    fill_out = sum(p == FILL for p in po) / max(1, len(po))
    colours = len(set(po))
    check(fill_out < 0.5 and colours > 8, f'output: the protected window is drawn ({fill_out:.0%} fill, {colours} colours)')
    oo = region(out, other)
    for name in ('screenshot', 'mirror', 'portal'):
        img = ppm(f'{d}/lordos-test-frame-{name}.ppm')
        check(img[0] == out[0] and img[1] == out[1], f'{name}: {img[0]}x{img[1]}, the output\'s size')
        pc = region(img, prot)
        grey = sum(p[0] == p[1] == p[2] for p in pc) / max(1, len(pc))
        fill = sum(p == FILL for p in pc) / max(1, len(pc))
        check(grey == 1.0 and fill > 0.9, f'{name}: the protected window is the fill ({fill:.1%} fill, {grey:.1%} neutral)')
        oc = region(img, other)
        same = sum(a == b for a, b in zip(oc, oo)) / max(1, len(oo))
        check(same > 0.97, f'{name}: the other window is captured as shown ({same:.1%} identical)')
    wp = ppm(f'{d}/lordos-test-frame-window-p.ppm')
    allp = region(wp, (0, 0, wp[0], wp[1]), inset=0)
    grey = sum(p[0] == p[1] == p[2] for p in allp) / max(1, len(allp))
    fill = sum(p == FILL for p in allp) / max(1, len(allp))
    check(grey == 1.0 and fill > 0.9, f'window share of the protected window: the fill only ({fill:.1%} fill, {grey:.1%} neutral)')
    wo = ppm(f'{d}/lordos-test-frame-window-o.ppm')
    allo = region(wo, (0, 0, wo[0], wo[1]), inset=0)
    fill = sum(p == FILL for p in allo) / max(1, len(allo))
    check(fill < 0.2 and len(set(allo)) > 8, f'window share of the other window: its own pixels ({fill:.1%} fill)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
