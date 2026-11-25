#!/usr/bin/env python3
# gen-mascot.py — Lumo, the LordOS mascot: the runtime PNGs the UI draws, made from the 3D masters.
#
#   python3 tools/gen-mascot.py                 write assets/mascot/*.png from the 1024 px masters
#   python3 tools/gen-mascot.py --import DIR    first copy new masters from DIR (lumo-<name>-{512,1024}.png),
#                                               recompressed losslessly, into assets/brand/mascot/png/
#   python3 tools/gen-mascot.py --check         only verify that assets/mascot/ is what the masters give (make test)
#   python3 tools/gen-mascot.py --preview OUT   also write a sheet of every render on a dark and a light background
#
# The masters (assets/brand/mascot/png/lumo-<name>-{512,1024}.png) are the 3D Lumo approved for 0.8.9.6.2:
# AI-generated raster art (no vector, model or rig), RGBA with a real transparent background. They are the
# source of truth and are never edited here; 1024 is the high-quality master, 512 the standard one.
#
# Every runtime size is made from the 1024 master with Lanczos on premultiplied alpha (colour times alpha is
# resized, then divided back), so the soft edges of the fur keep their own colour: a plain resize mixes in the
# black or white of fully transparent pixels and leaves a dark or light halo on the other background. Two
# clean-ups, for the derived files only: the generator left the body at alpha 250–254 (about 1 % see-through),
# which becomes 255, and alpha below 3 (invisible specks) becomes 0, which also compresses better.
#
# The avatar has a second, tighter render ("avatar-small", up to 40 px): the same picture cropped to the head
# and the top of the cape, without the transparent margin, so the face is as big as the box allows.
#
# Which file the UI draws for a size and a display scale is decided by sdk/mascot.js (lumoSource); the sizes
# here are the ones it can choose from. See docs/LORDOS-BRAND.md ("Lumo") and docs/brand/README.md.
# Needs Pillow and NumPy (apt install python3-pil python3-numpy).
import io, os, sys

import numpy as np
from PIL import Image

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
MASTERS = os.path.join(REPO, 'assets/brand/mascot/png')
UI = os.path.join(REPO, 'assets/mascot')

VIEWS = ('front', 'three-quarter', 'back', 'avatar')
POSES = ('wave', 'shield', 'run', 'sleep', 'celebrate', 'confused')
NAMES = VIEWS + POSES

# what the UI draws (sdk/mascot.js keeps the same lists): the poses from 40 to 128 px at 100–300 % scale,
# the avatar at 24 px (About) and larger
UI_SIZES = {p: (128, 256, 512) for p in POSES}
UI_SIZES['avatar'] = (128, 256)
UI_SIZES['avatar-small'] = (32, 64)
# the avatar-small crop on the 1024 master: the head and crown edge to edge, the cape cut under the brooch
AVATAR_SMALL_CROP = (80, 40, 944, 904)


def load(name):
    im = Image.open(os.path.join(MASTERS, f'lumo-{name}-1024.png'))
    if im.mode != 'RGBA' or im.size != (1024, 1024):
        sys.exit(f'lumo-{name}-1024.png: expected a 1024 x 1024 RGBA PNG, got {im.mode} {im.size}')
    return im


def resize(im, px):
    """Lanczos on premultiplied alpha; returns an 8-bit RGBA image px x px."""
    a = np.asarray(im, dtype=np.float32) / 255.0
    a[..., :3] *= a[..., 3:4]
    ch = [np.asarray(Image.fromarray(np.ascontiguousarray(a[..., i]), 'F').resize((px, px), Image.LANCZOS)) for i in range(4)]
    q = np.clip(np.stack(ch, -1), 0.0, 1.0)
    al = q[..., 3:4]
    rgb = np.where(al > 1e-6, q[..., :3] / np.maximum(al, 1e-6), 0.0)
    out = (np.concatenate([np.clip(rgb, 0.0, 1.0), al], -1) * 255.0 + 0.5).astype(np.uint8)
    alpha = out[..., 3]
    alpha[alpha >= 250] = 255
    gone = alpha < 3
    alpha[gone] = 0
    out[gone] = 0
    return Image.fromarray(out, 'RGBA')


def png_bytes(im):
    b = io.BytesIO()
    im.save(b, 'PNG', optimize=True, compress_level=9)
    return b.getvalue()


def renders():
    """{file name: PNG bytes} of everything assets/mascot/ should hold."""
    out = {}
    for name, sizes in UI_SIZES.items():
        src = load('avatar').crop(AVATAR_SMALL_CROP) if name == 'avatar-small' else load(name)
        for px in sizes:
            out[f'lumo-{name}-{px}.png'] = png_bytes(resize(src, px))
    return out


def import_masters(src_dir):
    """Copy lumo-<name>-{512,1024}.png from src_dir, recompressed losslessly (pixels and alpha unchanged)."""
    os.makedirs(MASTERS, exist_ok=True)
    for name in NAMES:
        for px in (512, 1024):
            f = f'lumo-{name}-{px}.png'
            im = Image.open(os.path.join(src_dir, f))
            im.load()
            if im.mode != 'RGBA' or im.size != (px, px):
                sys.exit(f'{f}: expected a {px} x {px} RGBA PNG, got {im.mode} {im.size}')
            raw = open(os.path.join(src_dir, f), 'rb').read()
            new = png_bytes(im)
            if not np.array_equal(np.asarray(Image.open(io.BytesIO(new))), np.asarray(im)):
                sys.exit(f'{f}: recompression changed pixels')
            data = new if len(new) < len(raw) else raw
            open(os.path.join(MASTERS, f), 'wb').write(data)
            print(f, len(raw), '->', len(data), 'bytes')


def preview(out_path, files):
    """Every render at its own size, on a dark and a light background, side by side."""
    ims = [(k, Image.open(io.BytesIO(v))) for k, v in sorted(files.items())]
    w = sum(im.width for _, im in ims) + 8 * (len(ims) + 1)
    h = max(im.height for _, im in ims)
    sheet = Image.new('RGBA', (w, 2 * h + 24), (128, 128, 128, 255))
    for row, bg in enumerate(((22, 22, 32, 255), (250, 250, 252, 255))):
        x = 8
        for _, im in ims:
            tile = Image.new('RGBA', im.size, bg)
            tile.alpha_composite(im)
            sheet.paste(tile, (x, 8 + row * (h + 8)))
            x += im.width + 8
    sheet.convert('RGB').save(out_path)
    print('preview in', out_path)


def main(args):
    if '--import' in args:
        import_masters(args[args.index('--import') + 1])
    files = renders()
    if '--check' in args:
        # a newer Pillow or NumPy may round a few pixels differently: up to 3 levels per channel is the same picture
        def same(f, data):
            path = os.path.join(UI, f)
            if not os.path.exists(path):
                return False
            have = np.asarray(Image.open(path).convert('RGBA'), dtype=np.int16)
            want = np.asarray(Image.open(io.BytesIO(data)), dtype=np.int16)
            return have.shape == want.shape and int(np.abs(have - want).max()) <= 3
        bad = [f for f, data in files.items() if not same(f, data)]
        stale = sorted(set(f for f in os.listdir(UI) if f.endswith('.png')) - set(files))
        if bad or stale:
            sys.exit('assets/mascot/ is out of date: ' + ' '.join(bad + stale) + ' (run python3 tools/gen-mascot.py)')
        print('assets/mascot/ matches the masters')
        return
    os.makedirs(UI, exist_ok=True)
    for f in os.listdir(UI):
        if f.endswith('.png') and f not in files:
            os.remove(os.path.join(UI, f))
    total = 0
    for f, data in sorted(files.items()):
        open(os.path.join(UI, f), 'wb').write(data)
        total += len(data)
        print(f, len(data), 'bytes')
    print(len(files), 'files,', total, 'bytes in', os.path.relpath(UI, REPO))
    if '--preview' in args:
        preview(args[args.index('--preview') + 1], files)


if __name__ == '__main__':
    main(sys.argv[1:])
