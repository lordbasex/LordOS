#!/usr/bin/env python3
"""gen-lbfonts.py - the fonts of LordBench's shaping workload (server/src/bench/lbui.c, docs/BENCHMARK.md).

lord-bench opens no files, so the fonts it shapes with are built into it (build/gen/lb_fonts.h, made by the
Makefile from server/src/bench/fonts). The Latin one is LordOS's own Inter (assets/fonts); the others are
Debian 13's Noto fonts subset to exactly the characters of the paragraphs in server/src/bench/lbtext.h,
keeping every OpenType layout feature and lookup those characters can reach, so the shaper gives the same
glyphs and positions it gives with the full font on that text. It is not the same work: the subsets have
smaller coverage tables and fewer lookups, and shape about 12 % faster than the full fonts (VM 116), so the
workload is comparable only with itself. Run it by hand when a paragraph changes (it is not part of
the build; needs fontTools, `pip install fonttools`, and the packages fonts-noto-core and fonts-noto-cjk):

    python3 tools/gen-lbfonts.py [--fonts DIR]

DIR defaults to /usr/share/fonts and must hold truetype/noto/NotoSans{Arabic,Devanagari,Thai}-Regular.ttf
and opentype/noto/NotoSansCJK-Regular.ttc (face 2, Simplified Chinese). The results are committed: every
build, on every machine, shapes with the same bytes.
"""
import argparse
import os
import re
import sys
import unicodedata

from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEXT = os.path.join(ROOT, 'server', 'src', 'bench', 'lbtext.h')
OUT = os.path.join(ROOT, 'server', 'src', 'bench', 'fonts')

# (output, source relative to --fonts, face index in a collection)
FONTS = [
    ('NotoSansArabic-LB.ttf', 'truetype/noto/NotoSansArabic-Regular.ttf', 0),
    ('NotoSansDevanagari-LB.ttf', 'truetype/noto/NotoSansDevanagari-Regular.ttf', 0),
    ('NotoSansThai-LB.ttf', 'truetype/noto/NotoSansThai-Regular.ttf', 0),
    ('NotoSansCJKsc-LB.otf', 'opentype/noto/NotoSansCJK-Regular.ttc', 2),
]


def paragraphs():
    src = open(TEXT, encoding='utf-8').read()
    body = src[src.index('lb_paragraphs'):]
    body = body[body.index('{') + 1:body.index('};')]
    out = re.findall(r'"([^"\\]*)"', body)
    if len(out) != 5:
        sys.exit(f'gen-lbfonts: expected 5 paragraphs in {TEXT}, found {len(out)}')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fonts', default='/usr/share/fonts')
    args = ap.parse_args()
    text = ''.join(paragraphs())
    # and what the shaper may turn them into: canonical and compatibility decompositions (Thai SARA AM is
    # shaped as NIKHAHIT + SARA AA, shape.c)
    text += unicodedata.normalize('NFD', text) + unicodedata.normalize('NFKD', text)
    chars = sorted({ord(c) for c in text})
    os.makedirs(OUT, exist_ok=True)
    for name, rel, index in FONTS:
        opts = subset.Options()
        opts.layout_features = ['*']        # every feature: the shaper picks what its script needs
        opts.layout_scripts = ['*']
        opts.name_IDs = ['*']                # keep the copyright, licence and version records
        opts.name_languages = ['*']
        opts.name_legacy = True
        opts.notdef_outline = True
        opts.hinting = False                 # shaping doesn't use hinting
        opts.font_number = index
        font = subset.load_font(os.path.join(args.fonts, rel), opts)
        sub = subset.Subsetter(opts)
        sub.populate(unicodes=chars)
        sub.subset(font)
        dst = os.path.join(OUT, name)
        subset.save_font(font, dst, opts)
        kept = TTFont(dst)
        print(f'{name}: {os.path.getsize(dst)} bytes, {kept["maxp"].numGlyphs} glyphs')


if __name__ == '__main__':
    main()
