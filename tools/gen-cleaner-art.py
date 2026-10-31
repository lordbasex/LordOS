#!/usr/bin/env python3
"""Cleaner's illustrations: our own glossy objects, written here as SVG (docs/brand/cleaner) and rendered to PNG
(assets/cleaner) with rsvg-convert on the maintainer's machine; not a build dependency. Colours are the brand's
(docs/brand/README.md). """
import pathlib, re, subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC, OUT = ROOT / 'docs/brand/cleaner', ROOT / 'assets/cleaner'
SRC.mkdir(parents=True, exist_ok=True)
OUT.mkdir(parents=True, exist_ok=True)

DEFS = '''<defs>
<linearGradient id="crown" x1="0" y1="0" x2=".7" y2="1"><stop offset="0" stop-color="#fff3c4"/><stop offset=".45" stop-color="#f7c04d"/><stop offset="1" stop-color="#c9761f"/></linearGradient>
<linearGradient id="band" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#c9761f"/><stop offset=".45" stop-color="#ffe19a"/><stop offset="1" stop-color="#c9761f"/></linearGradient>
<linearGradient id="gold" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#FFF1A8"/><stop offset=".5" stop-color="#FFD60A"/><stop offset="1" stop-color="#E08A00"/></linearGradient>
<radialGradient id="gem" cx=".35" cy=".3"><stop offset="0" stop-color="#f7d9ff"/><stop offset=".6" stop-color="#a13be0"/><stop offset="1" stop-color="#5a1aa8"/></radialGradient>
<radialGradient id="gemc" cx=".35" cy=".3"><stop offset="0" stop-color="#e6fbff"/><stop offset=".6" stop-color="#39c5ff"/><stop offset="1" stop-color="#2a4fc8"/></radialGradient>
<linearGradient id="plinth" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#8fa0e8"/><stop offset="1" stop-color="#39467e"/></linearGradient>
<linearGradient id="steel" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff"/><stop offset=".6" stop-color="#d7def8"/><stop offset="1" stop-color="#8fa0e8"/></linearGradient>
<linearGradient id="deep" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#5a5cff"/><stop offset="1" stop-color="#171c4d"/></linearGradient>
<linearGradient id="violet" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#8a6bff"/><stop offset="1" stop-color="#39308f"/></linearGradient>
<linearGradient id="cyan" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#7fe3ff"/><stop offset="1" stop-color="#2a6fe0"/></linearGradient>
<linearGradient id="gloss" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".75"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>
<radialGradient id="lens" cx=".4" cy=".35"><stop offset="0" stop-color="#e6fbff" stop-opacity=".85"/><stop offset="1" stop-color="#39c5ff" stop-opacity=".35"/></radialGradient>
<linearGradient id="pane" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#ffffff" stop-opacity=".34"/><stop offset=".5" stop-color="#9fd8ff" stop-opacity=".16"/><stop offset="1" stop-color="#8a6bff" stop-opacity=".3"/></linearGradient>
<radialGradient id="back" cx=".5" cy=".5"><stop offset="0" stop-color="#bfeeff" stop-opacity=".75"/><stop offset=".55" stop-color="#7a7dff" stop-opacity=".3"/><stop offset="1" stop-color="#7a7dff" stop-opacity="0"/></radialGradient>
<radialGradient id="well" cx=".5" cy=".42"><stop offset="0" stop-color="#2a2f8c" stop-opacity=".1"/><stop offset="1" stop-color="#050828" stop-opacity=".62"/></radialGradient>
<linearGradient id="edgelit" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".95"/><stop offset=".45" stop-color="#bfeeff" stop-opacity=".25"/><stop offset="1" stop-color="#8a6bff" stop-opacity=".7"/></linearGradient>
<filter id="contact" x="-30%" y="-100%" width="160%" height="300%"><feGaussianBlur stdDeviation="7"/></filter>
<filter id="soft" x="-30%" y="-30%" width="160%" height="160%"><feGaussianBlur stdDeviation="12"/></filter>
<filter id="lift" x="-20%" y="-20%" width="140%" height="150%"><feDropShadow dx="0" dy="12" stdDeviation="10" flood-color="#060920" flood-opacity=".5"/></filter>
</defs>'''


def spark(x, y, r, o=1):
    k = r * .22
    return f'<path d="M{x} {y-r}Q{x+k} {y-k} {x+r} {y}Q{x+k} {y+k} {x} {y+r}Q{x-k} {y+k} {x-r} {y}Q{x-k} {y-k} {x} {y-r}Z" fill="#fff" opacity="{o}"/>'

def disc(cy, rx, ry, h, fill):
    return (f'<path d="M{220-rx} {cy}v{h}a{rx} {ry} 0 0 0 {rx*2} 0v-{h}z" fill="url(#deep)"/>'
            f'<ellipse cx="220" cy="{cy}" rx="{rx}" ry="{ry}" fill="url(#{fill})"/>'
            f'<ellipse cx="{220-rx*.28}" cy="{cy-ry*.3}" rx="{rx*.5}" ry="{ry*.4}" fill="url(#gloss)"/>')


CROWN = ('<g filter="url(#lift)">'
         # the far side of the crown: its inside shows, so it reads as a ring and not as a cut-out
         '<ellipse cx="220" cy="236" rx="132" ry="30" fill="#6b3410"/><path d="M104 232L132 150l44 44 44-78 44 78 44-44 28 82z" fill="#8a4a16"/>'
         '<path d="M92 240L60 96l90 74 70-136 70 136 90-74-32 144Q220 300 92 240z" fill="url(#crown)"/>'
         # each point lit on one side and shaded on the other
         '<path d="M220 34l-70 136 70 96z" fill="#fff" opacity=".3"/><path d="M220 34l70 136-70 96z" fill="#8a4210" opacity=".3"/>'
         '<path d="M60 96l90 74-26 84-32-14z" fill="#fff" opacity=".16"/><path d="M380 96l-90 74 26 84 32-14z" fill="#8a4210" opacity=".34"/>'
         # the band follows the curve of the ring
         '<path d="M90 236Q220 298 350 236L344 266Q220 328 96 266z" fill="url(#band)"/>'
         '<path d="M94 241Q220 301 346 241" fill="none" stroke="#fff" stroke-opacity=".75" stroke-width="4" stroke-linecap="round"/>'
         '<path d="M97 264Q220 326 343 264" fill="none" stroke="#7a3f10" stroke-opacity=".6" stroke-width="3" stroke-linecap="round"/>'
         '<circle cx="60" cy="92" r="15" fill="url(#gold)"/><circle cx="220" cy="30" r="17" fill="url(#gold)"/><circle cx="380" cy="92" r="15" fill="url(#gold)"/>'
         '<circle cx="55" cy="87" r="5" fill="#fff" opacity=".85"/><circle cx="214" cy="24" r="6" fill="#fff" opacity=".85"/><circle cx="375" cy="87" r="5" fill="#fff" opacity=".85"/>'
         '<path d="M220 158l26 22-10 34h-32l-10-34z" fill="url(#gem)"/><path d="M220 158l26 22h-52z" fill="#fff" opacity=".4"/><path d="M204 214l16-34 16 34z" fill="#3b0f78" opacity=".35"/>'
         '<circle cx="136" cy="262" r="11" fill="url(#gemc)"/><circle cx="304" cy="262" r="11" fill="url(#gemc)"/><circle cx="220" cy="284" r="13" fill="url(#gem)"/>'
         '<circle cx="132" cy="258" r="3.5" fill="#fff"/><circle cx="300" cy="258" r="3.5" fill="#fff"/><circle cx="215" cy="279" r="4" fill="#fff"/></g>')
ART = {
 'scan': disc(344, 112, 24, 30, 'plinth') + disc(316, 84, 17, 16, 'plinth') + '<ellipse cx="220" cy="312" rx="96" ry="13" fill="#060920" opacity=".55" filter="url(#contact)"/>' + CROWN,
 'cleanup': '<g transform="rotate(24 220 210)" filter="url(#lift)">'
   '<rect x="208" y="28" width="24" height="232" rx="12" fill="url(#steel)"/><rect x="212" y="34" width="6" height="210" rx="3" fill="#fff" opacity=".6"/>'
   '<rect x="174" y="236" width="92" height="42" rx="12" fill="url(#band)"/><rect x="182" y="240" width="76" height="12" rx="6" fill="url(#gloss)"/>'
   '<path d="M176 272H264L300 378Q220 402 140 378Z" fill="url(#crown)"/>'
   '<path d="M196 278L176 382M220 278V392M244 278L264 382" stroke="#9A4F12" stroke-width="4" stroke-linecap="round" opacity=".5"/>'
   '<path d="M186 278L164 366Q176 340 200 282Z" fill="#fff" opacity=".42"/></g>' + spark(112, 112, 16) + spark(84, 214, 9, .85) + spark(330, 92, 11, .9),
 'large': '<g filter="url(#lift)"><rect x="96" y="120" width="160" height="210" rx="20" fill="url(#deep)"/><rect x="134" y="84" width="170" height="226" rx="22" fill="url(#steel)"/>'
   '<rect x="134" y="84" width="170" height="90" rx="22" fill="url(#gloss)"/><rect x="160" y="120" width="96" height="12" rx="6" fill="#39467e"/><rect x="160" y="148" width="118" height="9" rx="4.5" fill="#7a86c0"/><rect x="160" y="168" width="84" height="9" rx="4.5" fill="#7a86c0"/>'
   '<circle cx="254" cy="240" r="72" fill="url(#lens)" stroke="url(#crown)" stroke-width="16"/><ellipse cx="232" cy="214" rx="26" ry="15" fill="#fff" opacity=".55"/>'
   '<path d="M308 294l58 58" stroke="url(#crown)" stroke-width="24" stroke-linecap="round"/></g>' + spark(84, 80, 13) + spark(380, 150, 10, .9),
 'storage': disc(306, 136, 40, 44, 'cyan') + disc(226, 136, 40, 44, 'violet') + disc(146, 136, 40, 44, 'steel')
   + '<ellipse cx="220" cy="146" rx="46" ry="13" fill="url(#deep)"/><ellipse cx="220" cy="143" rx="16" ry="5" fill="url(#gold)"/>' + spark(372, 92, 13) + spark(68, 118, 9, .85),
 'duplicates': '<g filter="url(#lift)"><rect x="82" y="86" width="186" height="240" rx="26" transform="rotate(-9 175 206)" fill="url(#violet)"/>'
   '<g transform="rotate(5 250 216)"><rect x="150" y="104" width="196" height="252" rx="26" fill="url(#steel)"/>'
   '<rect x="150" y="104" width="196" height="96" rx="26" fill="url(#gloss)"/>'
   '<rect x="178" y="140" width="96" height="14" rx="7" fill="#39467e"/><rect x="178" y="170" width="140" height="10" rx="5" fill="#7a86c0"/><rect x="178" y="192" width="118" height="10" rx="5" fill="#7a86c0"/>'
   '<circle cx="248" cy="282" r="44" fill="url(#crown)"/><path d="M226 282l16 16 30-34" fill="none" stroke="#fff" stroke-width="11" stroke-linecap="round" stroke-linejoin="round"/></g></g>' + spark(72, 70, 13) + spark(372, 84, 10, .9),
 'activity': disc(346, 92, 20, 22, 'plinth') + '<g filter="url(#lift)"><circle cx="220" cy="186" r="134" fill="url(#steel)"/><circle cx="220" cy="186" r="108" fill="url(#deep)"/>'
   '<ellipse cx="192" cy="118" rx="76" ry="38" fill="url(#gloss)" opacity=".55"/>'
   + ''.join(f'<rect x="216" y="88" width="8" height="20" rx="4" fill="#7fe3ff" opacity=".85" transform="rotate({a} 220 186)"/>' for a in range(0, 360, 30))
   + '<rect x="214" y="122" width="12" height="74" rx="6" fill="#fff"/><rect x="214" y="180" width="12" height="62" rx="6" fill="url(#crown)" transform="rotate(118 220 186)"/><circle cx="220" cy="186" r="13" fill="url(#crown)"/></g>' + spark(376, 116, 11) + spark(66, 128, 8, .85),
 'settings': '<g filter="url(#lift)"><path d="M220 44L348 92V206C348 288 296 344 220 374C144 344 92 288 92 206V92Z" fill="url(#steel)"/>'
   '<path d="M220 76L320 114V206C320 272 280 318 220 344C160 318 120 272 120 206V114Z" fill="url(#deep)"/>'
   '<path d="M220 76L320 114V170Q222 200 120 150V114Z" fill="url(#gloss)" opacity=".4"/>'
   '<path d="M170 208l36 36 66-74" fill="none" stroke="url(#crown)" stroke-width="20" stroke-linecap="round" stroke-linejoin="round"/></g>' + spark(380, 96, 12) + spark(58, 160, 9, .85),
}

# each object sits inside a thick piece of glass of its own shape
GLASS = {'scan': 'M220 18L392 112a24 24 0 0 1 12 21v154a24 24 0 0 1-12 21L220 402 48 308a24 24 0 0 1-12-21V133a24 24 0 0 1 12-21z',
         'cleanup': 'M96 30h248a66 66 0 0 1 66 66v228a66 66 0 0 1-66 66H96a66 66 0 0 1-66-66V96a66 66 0 0 1 66-66z',
         'large': 'M220 22a188 188 0 1 0 0 376 188 188 0 0 0 0-376z',
         'storage': 'M120 26h200a84 84 0 0 1 84 84v200a84 84 0 0 1-84 84H120a84 84 0 0 1-84-84V110a84 84 0 0 1 84-84z',
         'duplicates': 'M110 46h232a60 60 0 0 1 60 60v208a60 60 0 0 1-60 60H98a60 60 0 0 1-60-60V118a72 72 0 0 1 72-72z',
         'activity': 'M220 22a188 188 0 1 0 0 376 188 188 0 0 0 0-376z',
         'settings': 'M220 18c30 26 98 44 168 48 14 150-40 290-168 336C92 356 38 216 52 66c70-4 138-22 168-48z'}

def glass(name):
    """The pane alone: dark enough inside for the object to stand on, lit on one edge, with a glow behind the object."""
    d = GLASS[name]
    return (f'<clipPath id="gc"><path d="{d}"/></clipPath><path d="{d}" fill="#0a1040" opacity=".34" transform="translate(0 16)" filter="url(#soft)"/>'
            f'<path d="{d}" fill="url(#pane)"/><path d="{d}" fill="url(#well)"/>'
            f'<g clip-path="url(#gc)"><path d="{d}" fill="none" stroke="#bfeeff" stroke-opacity=".6" stroke-width="26" filter="url(#soft)"/>'
            '<ellipse cx="220" cy="210" rx="170" ry="150" fill="url(#back)"/><ellipse cx="170" cy="54" rx="190" ry="70" fill="url(#gloss)" opacity=".42"/></g>'
            f'<path d="{d}" fill="none" stroke="url(#edgelit)" stroke-width="4"/>')

def object_(body):
    return f'<g transform="translate(62 54) scale(.72)">{body}</g>'

# the tiles: a colour and a white line glyph
TILES = {'scan': ('#b14bff', 'M4 7V5a1 1 0 011-1h2M17 4h2a1 1 0 011 1v2M20 17v2a1 1 0 01-1 1h-2M7 20H5a1 1 0 01-1-1v-2M4 12h16'),
         'cleanup': ('#ff5fa2', 'M9 15l-5 5M14 4l6 6-8 8-6-6z'), 'storage': ('#2fa8ff', 'M4 5h7v7H4zM13 5h7v14h-7zM4 14h7v5H4z'),
         'large': ('#f7a81b', 'M11 4a7 7 0 100 14 7 7 0 000-14zM16 16l5 5'), 'duplicates': ('#ff9f3d', 'M9 9h11v11H9zM5 15H4V4h11v1'),
         'activity': ('#22c28f', 'M3 12h4l3 7 4-14 3 7h4'), 'settings': ('#7a7fa8', 'M4 7h10M18 7h2M4 17h2M10 17h10M14 5v4M6 15v4')}

def tile(colour, d):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">'
            f'<rect width="64" height="64" rx="20" fill="{colour}"/><rect width="64" height="32" rx="20" fill="#fff" opacity=".12"/>'
            f'<g transform="translate(14 14) scale(1.5)"><path d="{d}" fill="none" stroke="#fff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></g></svg>')

# the storage bubbles' folders: what the folder is, not a repeated icon
FOLDERS = {'documents': ('#3b82f6', 'M7 3h7l4 4v14H7zM14 3v4h4M10 12h5M10 16h5'), 'pictures': ('#ec4899', 'M4 5h16v14H4zM4 16l5-5 4 4 3-3 4 4M15 9h.01'),
           'music': ('#a855f7', 'M9 18V6l10-2v12M9 18a2.5 2.5 0 11-5 0 2.5 2.5 0 015 0zM19 16a2.5 2.5 0 11-5 0 2.5 2.5 0 015 0z'), 'videos': ('#ef4444', 'M4 5h16v14H4zM10 9l5 3-5 3z'),
           'downloads': ('#10b981', 'M12 4v11M7 11l5 5 5-5M5 20h14'), 'desktop': ('#6366f1', 'M3 5h18v11H3zM9 20h6M12 16v4'),
           'code': ('#f59e0b', 'M8 8l-4 4 4 4M16 8l4 4-4 4M14 5l-4 14'), 'cache': ('#fb7185', 'M13 3L5 14h6l-1 7 8-11h-6z'),
           'config': ('#94a3b8', 'M4 7h10M18 7h2M4 17h2M10 17h10M14 5v4M6 15v4'), 'data': ('#14b8a6', 'M5 6c0-3 14-3 14 0v12c0 3-14 3-14 0zM5 6c0 3 14 3 14 0M5 12c0 3 14 3 14 0'),
           'trash': ('#78716c', 'M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13M10 11v6M14 11v6'), 'folder': ('#38bdf8', 'M3 6h6l2 3h10v10H3z'), 'others': ('#71717a', 'M6 12h.01M12 12h.01M18 12h.01')}

# a glass sphere: colour at the rim, nearly clear in the middle for the text, one highlight. Stretched to each bubble.
HUES = {'violet': '#8a4bff', 'blue': '#2f8bff', 'pink': '#ff4fa8', 'teal': '#17c3b2', 'orange': '#ff8a3d', 'rose': '#f0436a', 'indigo': '#6366f1', 'sky': '#38bdf8'}
def bubble(colour):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="360" height="360" viewBox="0 0 360 360"><defs>'
            f'<radialGradient id="b" cx=".5" cy=".46" r=".54"><stop offset="0" stop-color="{colour}" stop-opacity=".2"/><stop offset=".72" stop-color="{colour}" stop-opacity=".34"/><stop offset=".95" stop-color="{colour}" stop-opacity=".82"/><stop offset="1" stop-color="{colour}" stop-opacity=".95"/></radialGradient>'
            '<radialGradient id="h" cx=".5" cy=".5"><stop offset="0" stop-color="#fff" stop-opacity=".7"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>'
            '<linearGradient id="e" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".9"/><stop offset=".5" stop-color="#fff" stop-opacity=".12"/><stop offset="1" stop-color="#fff" stop-opacity=".4"/></linearGradient>'
            f'</defs><circle cx="180" cy="180" r="176" fill="url(#b)"/><circle cx="180" cy="180" r="175" fill="none" stroke="url(#e)" stroke-width="3"/>'
            '<ellipse cx="116" cy="84" rx="66" ry="36" fill="url(#h)" transform="rotate(-34 116 84)"/><ellipse cx="256" cy="288" rx="46" ry="16" fill="url(#h)" opacity=".4" transform="rotate(-38 256 288)"/></svg>')

PARTS = {'spark': (48, 48, '<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" viewBox="0 0 48 48"><path d="M24 2Q27 21 46 24Q27 27 24 46Q21 27 2 24Q21 21 24 2Z" fill="#fff"/></svg>'),
         'shadow': (220, 44, '<svg xmlns="http://www.w3.org/2000/svg" width="220" height="44" viewBox="0 0 220 44"><defs><filter id="s" x="-20%" y="-100%" width="140%" height="300%"><feGaussianBlur stdDeviation="9"/></filter></defs><ellipse cx="110" cy="22" rx="84" ry="10" fill="#050828" filter="url(#s)"/></svg>'),
         'orbit-dot-dark': (28, 28, '<svg xmlns="http://www.w3.org/2000/svg" width="28" height="28" viewBox="0 0 28 28"><circle cx="14" cy="14" r="14" fill="#7fe3ff" opacity=".25"/><circle cx="14" cy="14" r="7" fill="#7fe3ff"/></svg>'),
         'orbit-dot-light': (28, 28, '<svg xmlns="http://www.w3.org/2000/svg" width="28" height="28" viewBox="0 0 28 28"><circle cx="14" cy="14" r="14" fill="#0a6fc2" opacity=".25"/><circle cx="14" cy="14" r="7" fill="#0a6fc2"/></svg>')}

def sky(a, b, c, d, name):
    s = ('<svg xmlns="http://www.w3.org/2000/svg" width="660" height="440" viewBox="0 0 660 440"><defs>'
         f'<linearGradient id="l" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{a}"/><stop offset=".6" stop-color="{b}"/><stop offset="1" stop-color="{c}"/></linearGradient>'
         f'<radialGradient id="r" gradientUnits="userSpaceOnUse" cx="528" cy="44" r="330"><stop offset="0" stop-color="{d}"/><stop offset=".62" stop-color="{d}" stop-opacity="0"/></radialGradient>'
         '</defs><rect width="660" height="440" fill="url(#l)"/><rect width="660" height="440" fill="url(#r)"/></svg>')
    p = SRC / f'{name}.svg'
    p.write_text(s + '\n')
    subprocess.check_call(['rsvg-convert', '-w', '660', '-h', '440', str(p), '-o', str(OUT / f'{name}.png')])

def orbits(accent, line, name):
    outer = (f'<svg xmlns="http://www.w3.org/2000/svg" width="340" height="340" viewBox="0 0 340 340"><circle cx="170" cy="170" r="162" fill="none" stroke="{accent}" stroke-opacity=".7" stroke-dasharray="3 5"/></svg>')
    inner = (f'<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256"><circle cx="128" cy="128" r="120" fill="none" stroke="{line}" stroke-opacity=".45"/></svg>')
    for n, s, w in (('outer', outer, 340), ('inner', inner, 256)):
        p = SRC / f'orbit-{n}-{name}.svg'
        p.write_text(s + '\n')
        subprocess.check_call(['rsvg-convert', '-w', str(w), '-h', str(w), str(p), '-o', str(OUT / f'orbit-{n}-{name}.png')])

for colour, name in (('#ffffff', 'status-light'), ('#141a4d', 'status-dark')):
    p = SRC / f'{name}.svg'
    p.write_text(f'<svg xmlns="http://www.w3.org/2000/svg" width="36" height="36" viewBox="0 0 24 24"><path d="M4 19h16M5 16 3.5 7l5 4 3.5-7 3.5 7 5-4L19 16z" fill="{colour}" stroke="{colour}" stroke-width="1.6" stroke-linejoin="round" stroke-linecap="round"/></svg>\n')
    subprocess.check_call(['rsvg-convert', '-w', '36', '-h', '36', str(p), '-o', str(OUT / f'{name}.png')])

sky('#060920', '#0d1138', '#1b1152', '#2c2a84', 'sky-dark')
sky('#f6f8ff', '#e8edff', '#dcd9ff', '#c9d4ff', 'sky-light')
orbits('#7fe3ff', '#96afff', 'dark')
orbits('#0a6fc2', '#3c50b4', 'light')

# the scan button's turning arc
RING = ('<svg xmlns="http://www.w3.org/2000/svg" width="160" height="160" viewBox="0 0 160 160"><defs><linearGradient id="a" x1="0" y1="0" x2="1" y2="1">'
        '<stop offset="0" stop-color="#7fe3ff"/><stop offset="1" stop-color="#8a6bff" stop-opacity="0"/></linearGradient></defs>'
        '<circle cx="80" cy="80" r="74" fill="none" stroke="#7fe3ff" stroke-opacity=".0" stroke-width="6"/>'
        '<path d="M80 6A74 74 0 0 1 154 80" fill="none" stroke="#7fe3ff" stroke-width="6" stroke-linecap="round"/>'
        '<path d="M154 80A74 74 0 0 1 80 154" fill="none" stroke="url(#a)" stroke-width="6" stroke-linecap="round"/></svg>')
(SRC / 'ring.svg').write_text(RING + '\n')
subprocess.check_call(['rsvg-convert', '-w', '160', '-h', '160', str(SRC / 'ring.svg'), '-o', str(OUT / 'ring.png')])

def png(name, w, h, svg):
    p = SRC / f'{name}.svg'
    p.write_text(svg + '\n')
    subprocess.check_call(['rsvg-convert', '-w', str(w), '-h', str(h), str(p), '-o', str(OUT / f'{name}.png')])

HEAD = '<svg xmlns="http://www.w3.org/2000/svg" width="440" height="420" viewBox="0 0 440 420">' + DEFS
for name, body in ART.items():
    png(f'hero-{name}', 440, 420, f'{HEAD}{glass(name)}{object_(body)}</svg>')   # scan screens and the dashboard
    png(f'glass-{name}', 440, 420, f'{HEAD}{glass(name)}</svg>')                 # a section's opening: pane and object apart,
    png(f'object-{name}', 440, 420, f'{HEAD}{object_(body)}</svg>')              # so the object floats over its pane
for name, (c, d) in TILES.items():
    png(f'tile-{name}', 64, 64, tile(c, d))
for name, (c, d) in FOLDERS.items():
    png(f'folder-{name}', 64, 64, tile(c, d))
for name, c in HUES.items():
    png(f'bubble-{name}', 360, 360, bubble(c))
for name, (w, h, svg) in PARTS.items():
    png(name, w, h, svg)
