#!/usr/bin/env python3
# gen-brand.py — the provisional Axia DE logo (assets/brand/*.svg) and its PNG renders (assets/icons/).
#
#   python3 tools/gen-brand.py          write the SVGs and render the PNGs (needs rsvg-convert:
#                                       `dnf install librsvg2-tools`)
#
# The mark is an "A" whose crossbar runs past both legs, like an axis. The app icon puts it in white
# on the violet rounded square the desktop already uses; the mono glyph is the mark alone, for a menu
# bar. Sizes of 32 px and less use heavier strokes so the mark stays readable on the pixel grid.
# The wordmark is live text in the UI (AxiaWordmark in sdk/logo.js), so no lockup is drawn here.
import os, subprocess

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
OUT = os.path.join(REPO, 'assets/brand')
ICONS = os.path.join(REPO, 'assets/icons')

VIOLET_LIGHT, VIOLET_DARK = '#B46CFF', '#4F46E5'
INK_DARK, INK_LIGHT = '#1d1d1f', '#ffffff'


def mark(fill, small=False):
    """The mark in a 512 box: two legs meeting at the apex and the long crossbar."""
    leg, bar = (76, 56) if small else (58, 38)
    return (f'<g fill="none" stroke="{fill}" stroke-linecap="round" stroke-linejoin="round">'
            f'<path d="M150 396L256 118L362 396" stroke-width="{leg}"/>'
            f'<path d="M108 296H404" stroke-width="{bar}"/></g>')


def svg(body, defs='', title='Axia DE'):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512">'
            f'<title>{title}</title><defs>{defs}</defs>{body}</svg>\n')


def app_icon(small=False):
    defs = (f'<linearGradient id="bg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{VIOLET_LIGHT}"/>'
            f'<stop offset="1" stop-color="{VIOLET_DARK}"/></linearGradient>'
            '<radialGradient id="gl" cx=".22" cy=".12" r=".75"><stop offset="0" stop-color="#fff" stop-opacity=".25"/>'
            '<stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>'
            # user-space gradients: a bounding-box one paints nothing on the flat crossbar
            '<linearGradient id="ink" gradientUnits="userSpaceOnUse" x1="0" y1="90" x2="0" y2="430"><stop offset="0" stop-color="#fff"/>'
            '<stop offset="1" stop-color="#EEEAFF"/></linearGradient>')
    body = ('<rect width="512" height="512" rx="112" fill="url(#bg)"/>'
            '<rect width="512" height="512" rx="112" fill="url(#gl)"/>'
            '<g transform="translate(256 262) scale(.78) translate(-256 -256)">' + mark('url(#ink)', small) + '</g>')
    return svg(body, defs)


def color_mark():
    defs = (f'<linearGradient id="mk" gradientUnits="userSpaceOnUse" x1="90" y1="90" x2="430" y2="430"><stop offset="0" stop-color="{VIOLET_LIGHT}"/>'
            f'<stop offset="1" stop-color="{VIOLET_DARK}"/></linearGradient>')
    return svg(mark('url(#mk)'), defs)


def write_svgs():
    os.makedirs(OUT, exist_ok=True)
    files = {
        'app-icon.svg': app_icon(),
        'app-icon-small.svg': app_icon(small=True),
        'mark.svg': color_mark(),
        'mark-mono.svg': svg(mark('#000')),
        'mark-mono-small.svg': svg(mark('#000', small=True)),
    }
    for name, text in files.items():
        with open(os.path.join(OUT, name), 'w') as f:
            f.write(text)


def render(src, dst, size, color=None):
    path = os.path.join(OUT, src)
    if color:
        # the mono glyph in the given ink, without touching the SVG on disk
        with open(path) as f:
            text = f.read().replace('stroke="#000"', f'stroke="{color}"')
        data = text.encode()
        subprocess.run(['rsvg-convert', '-w', str(size), '-h', str(size), '-o', dst, '/dev/stdin'], input=data, check=True)
    else:
        subprocess.run(['rsvg-convert', '-w', str(size), '-h', str(size), '-o', dst, path], check=True)


RENDERS = [
    ('axia-mark.png', 'mark.svg', 256, None),
    ('axia-mark-512.png', 'mark.svg', 512, None),
    *[(f'axia-icon-{s}.png', 'app-icon-small.svg' if s <= 32 else 'app-icon.svg', s, None)
      for s in (16, 32, 64, 128, 256, 512, 1024)],
    *[(f'axia-glyph-{tone}-{s}.png', 'mark-mono-small.svg' if s <= 16 else 'mark-mono.svg', s, col)
      for tone, col in (('dark', INK_DARK), ('light', INK_LIGHT)) for s in (16, 32)],
]


def main():
    write_svgs()
    os.makedirs(ICONS, exist_ok=True)
    for dst, src, size, color in RENDERS:
        render(src, os.path.join(ICONS, dst), size, color)
        print(f'{dst} ({size} px)')


if __name__ == '__main__':
    main()
