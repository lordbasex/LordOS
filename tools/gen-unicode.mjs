// gen-unicode.mjs — generates server/src/ui/unidata.h from the Unicode Character Database
// (UCD) files. These are range tables for:
//   - bidi class (UAX #9), mirrored pairs and bracket pairs → bidi.c
//   - joining type for Arabic, Syriac, N'Ko, Mongolian… (ArabicShaping) → init/medi/fina forms
//   - script (Scripts.txt) → OpenType tag and fallback font
//   - combining class (canonical mark order) and default-ignorable characters
//   - Indic categories (syllabic and positional, grouped the way HarfBuzz does), the Khmer and
//     Myanmar ones, the Universal Shaping Engine ones (Tibetan, Sinhala, Balinese, Javanese…), marks,
//     canonical decompositions of the complex scripts and composition exclusions
//   - line-break classes that matter for Chinese, Japanese and Korean (UAX #14)
//   - grapheme cluster breaks (UAX #29): what the caret steps over as one character → grapheme.c
//
//   node tools/gen-unicode.mjs <UCD folder>
// The folder needs (from https://www.unicode.org/Public/<version>/ucd/):
//   extracted/DerivedBidiClass.txt  extracted/DerivedJoiningType.txt  BidiMirroring.txt
//   BidiBrackets.txt  Scripts.txt  PropertyValueAliases.txt  LineBreak.txt
//   extracted/DerivedCombiningClass.txt  DerivedCoreProperties.txt
//   IndicSyllabicCategory.txt  IndicPositionalCategory.txt  UnicodeData.txt  CompositionExclusions.txt
//   ArabicShaping.txt, auxiliary/GraphemeBreakProperty.txt, emoji/emoji-data.txt, and from HarfBuzz (src/ms-use/): IndicSyllabicCategory-Additional.txt
//   IndicPositionalCategory-Additional.txt (Universal Shaping Engine categories)
// (the extracted/, auxiliary/ and emoji/ ones may be in the same folder).
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const ucd = process.argv[2];
if (!ucd) { console.error('usage: gen-unicode.mjs <UCD folder>'); process.exit(1); }
const read = (name) => {
  for (const p of [path.join(ucd, name), path.join(ucd, 'extracted', name), path.join(ucd, 'auxiliary', name), path.join(ucd, 'emoji', name)])
    if (fs.existsSync(p)) return fs.readFileSync(p, 'utf8');
  throw new Error(`missing ${name}`);
};
const version = (read('DerivedBidiClass.txt').match(/DerivedBidiClass-([\d.]+)\.txt/) || [])[1] || '?';

// Lines "XXXX..YYYY ; value" (and the @missing ones, which give the default values).
function parse(text, { missing = true } = {}) {
  const out = [];
  for (const line of text.split('\n')) {
    let m = missing && line.match(/^# @missing: ([0-9A-F]+)\.\.([0-9A-F]+); (\S+)/);
    if (m) { out.push({ lo: parseInt(m[1], 16), hi: parseInt(m[2], 16), v: m[3], miss: true }); continue; }
    const body = line.split('#')[0].trim();
    if (!body) continue;
    const f = body.split(';').map((s) => s.trim());
    const [a, b] = f[0].split('..');
    out.push({ lo: parseInt(a, 16), hi: parseInt(b || a, 16), v: f[1], f });
  }
  return out;
}

// Assigns values to a sparse range map: first the @missing ones (from most general to most
// specific, as they appear in the file), then the explicit ones.
function ranges(entries, map, deflt) {
  const val = new Map();
  const set = (lo, hi, v) => { for (let c = lo; c <= hi; c++) val.set(c, v); };
  for (const e of entries.filter((e) => e.miss)) set(e.lo, e.hi, map(e.v));
  for (const e of entries.filter((e) => !e.miss)) set(e.lo, e.hi, map(e.v));
  const out = [];
  let cur = null;
  for (let c = 0; c <= 0x10ffff; c++) {
    const v = val.has(c) ? val.get(c) : deflt;
    if (cur && cur.v === v && cur.hi === c - 1) cur.hi = c;
    else { if (cur && cur.v !== deflt) out.push(cur); cur = { lo: c, hi: c, v }; }
  }
  if (cur && cur.v !== deflt) out.push(cur);
  return out;
}

const hex = (n) => `0x${n.toString(16).toUpperCase()}`;
// The enums go on top; the tables are only compiled in unicode.c (UNIDATA_TABLES).
let enums = '';
let h = '';

// ---- bidi ----
const BIDI = ['L', 'R', 'AL', 'EN', 'ES', 'ET', 'AN', 'CS', 'NSM', 'BN', 'B', 'S', 'WS', 'ON',
  'LRE', 'LRO', 'RLE', 'RLO', 'PDF', 'LRI', 'RLI', 'FSI', 'PDI'];
const BIDI_LONG = { Left_To_Right: 'L', Right_To_Left: 'R', Arabic_Letter: 'AL', European_Terminator: 'ET',
  Boundary_Neutral: 'BN' };
const bidi = ranges(parse(read('DerivedBidiClass.txt')), (v) => BIDI.indexOf(BIDI_LONG[v] || v), 0);
enums += `enum { ${BIDI.map((c) => `BIDI_${c}`).join(', ')} };\n`;
h += `static const UniRange uni_bidi[] = {\n${bidi.map((r) => `    { ${hex(r.lo)}, ${hex(r.hi)}, BIDI_${BIDI[r.v]} },`).join('\n')}\n};\n\n`;

const mirror = parse(read('BidiMirroring.txt'), { missing: false }).map((e) => ({ a: e.lo, b: parseInt(e.f[1], 16) }));
h += `static const UniPair uni_mirror[] = {\n${mirror.map((p) => `    { ${hex(p.a)}, ${hex(p.b)} },`).join('\n')}\n};\n\n`;

const brackets = parse(read('BidiBrackets.txt'), { missing: false }).map((e) => ({ a: e.lo, b: parseInt(e.f[1], 16), open: e.f[2] === 'o' }));
h += `static const UniBracket uni_brackets[] = {\n${brackets.map((p) => `    { ${hex(p.a)}, ${hex(p.b)}, ${p.open ? 1 : 0} },`).join('\n')}\n};\n\n`;

// ---- joining (Arabic and other joining scripts) ----
const JOIN = ['U', 'R', 'L', 'D', 'C', 'T'];
const JOIN_LONG = { Non_Joining: 'U', Right_Joining: 'R', Left_Joining: 'L', Dual_Joining: 'D', Join_Causing: 'C', Transparent: 'T' };
const join = ranges(parse(read('DerivedJoiningType.txt')), (v) => JOIN.indexOf(JOIN_LONG[v] || v), 0);
enums += `enum { ${JOIN.map((c) => `JOIN_${c}`).join(', ')} };\n`;
h += `static const UniRange uni_join[] = {\n${join.map((r) => `    { ${hex(r.lo)}, ${hex(r.hi)}, JOIN_${JOIN[r.v]} },`).join('\n')}\n};\n\n`;

// ---- scripts: ISO 15924 code and OpenType tag ----
const alias = {}, longName = {};
for (const line of read('PropertyValueAliases.txt').split('\n')) {
  const m = line.match(/^sc\s*;\s*(\w+)\s*;\s*(\w+)/);
  if (m) { alias[m[2]] = m[1]; longName[m[1]] = m[2]; }
}
const scripts = ['Zyyy'];                       // 0 = Common
const sidx = (name) => {
  const code = name === 'Common' ? 'Zyyy' : alias[name] || name;
  let i = scripts.indexOf(code);
  if (i < 0) { scripts.push(code); i = scripts.length - 1; }
  return i;
};
sidx('Inherited');                              // 1 = Inherited
const script = ranges(parse(read('Scripts.txt'), { missing: false }), sidx, 0);
// OpenType tags: the lowercase code, except those the specification defines differently.
const OT = { Zyyy: 'DFLT', Zinh: 'DFLT', Hira: 'kana', Kana: 'kana', Hrkt: 'kana', Hani: 'hani', Hang: 'hang', Laoo: 'lao ',
  Yiii: 'yi  ', Nkoo: 'nko ', Vaii: 'vai ' };
enums += `#define SCRIPT_COMMON 0\n#define SCRIPT_INHERITED 1\n`;
for (const k of ['Latn', 'Grek', 'Cyrl', 'Arab', 'Hebr', 'Syrc', 'Thaa', 'Nkoo', 'Mong', 'Hani', 'Hira', 'Kana', 'Hang', 'Thai', 'Laoo', 'Khmr', 'Mymr'])
  enums += `#define SCRIPT_${k.toUpperCase()} ${scripts.indexOf(k)}\n`;
enums += `#define SCRIPT_COUNT ${scripts.length}\n`;
h += `static const char uni_script_tag[][5] = {\n${scripts.map((s) => `    "${OT[s] || s.toLowerCase()}",   /* ${s} */`).join('\n')}\n};\n\n`;
// English name with spaces (to look up the "Noto Sans <name>" font).
h += `static const char *const uni_script_name[] = {\n${scripts.map((s) => `    "${(longName[s] || s).replace(/_/g, ' ')}",`).join('\n')}\n};\n\n`;
h += `static const UniRange uni_script[] = {\n${script.map((r) => `    { ${hex(r.lo)}, ${hex(r.hi)}, ${r.v} },`).join('\n')}\n};\n\n`;

// ---- line breaking: only what scripts without spaces need (simplified UAX #14) ----
// ID: can break before and after (ideographs, kana, hangul syllables).
// CL: no break before (closing, 、。). OP: no break after (opening). NS: no break
// before (ー, small ぁ, ・). EX/IS: no break before (！？ and separators). SA: Southeast Asian
// scripts without spaces between words (Thai, Lao, Khmer, Myanmar…): broken
// by dictionary (Thai) or by syllable (textlayout.c).
const LB = ['XX', 'ID', 'CL', 'OP', 'NS', 'EX', 'IS', 'SA'];
const LB_MAP = { ID: 'ID', H2: 'ID', H3: 'ID', JL: 'ID', JV: 'ID', JT: 'ID', CL: 'CL', CP: 'CL', OP: 'OP', NS: 'NS', CJ: 'NS',
  EX: 'EX', IS: 'IS', SA: 'SA' };
const lb = ranges(parse(read('LineBreak.txt')), (v) => LB.indexOf(LB_MAP[v] || 'XX'), 0);
enums += `enum { ${LB.map((c) => `LB_${c}`).join(', ')} };\n`;
h += `static const UniRange uni_linebreak[] = {\n${lb.map((r) => `    { ${hex(r.lo)}, ${hex(r.hi)}, LB_${LB[r.v]} },`).join('\n')}\n};\n\n`;

// ---- marks: canonical combining class; invisibles (ZWJ, direction marks…) ----
const ccc = ranges(parse(read('DerivedCombiningClass.txt')), (v) => Number(v) || 0, 0);
h += `static const UniRange uni_ccc[] = {\n${ccc.map((r) => `    { ${hex(r.lo)}, ${hex(r.hi)}, ${r.v} },`).join('\n')}\n};\n\n`;
const ign = ranges(parse(read('DerivedCoreProperties.txt'), { missing: false }).filter((e) => e.v === 'Default_Ignorable_Code_Point'), () => 1, 0);
h += `static const UniRange uni_ignorable[] = {\n${ign.map((r) => `    { ${hex(r.lo)}, ${hex(r.hi)}, 1 },`).join('\n')}\n};\n\n`;

// ---- grapheme clusters (UAX #29, extended): Grapheme_Cluster_Break, Extended_Pictographic (emoji
// ZWJ sequences) and Indic_Conjunct_Break (conjuncts joined by a virama) → grapheme.c ----
const GCB = ['Other', 'CR', 'LF', 'Control', 'Extend', 'ZWJ', 'RI', 'Prepend', 'SpacingMark', 'L', 'V', 'T', 'LV', 'LVT'];
const GCB_LONG = { Regional_Indicator: 'RI' };
const gcb = ranges(parse(read('GraphemeBreakProperty.txt'), { missing: false }), (v) => GCB.indexOf(GCB_LONG[v] || v), 0);
enums += `enum { ${GCB.map((c) => `GCB_${c}`).join(', ')} };\n`;
h += `static const UniRange uni_gcb[] = {\n${gcb.map((r) => `    { ${hex(r.lo)}, ${hex(r.hi)}, GCB_${GCB[r.v]} },`).join('\n')}\n};\n\n`;
const extpict = ranges(parse(read('emoji-data.txt'), { missing: false }).filter((e) => e.v === 'Extended_Pictographic'), () => 1, 0);
h += `static const UniRange uni_extpict[] = {\n${extpict.map((r) => `    { ${hex(r.lo)}, ${hex(r.hi)}, 1 },`).join('\n')}\n};\n\n`;
const INCB = ['None', 'Linker', 'Consonant', 'Extend'];
const incbR = ranges(parse(read('DerivedCoreProperties.txt'), { missing: false }).filter((e) => e.v === 'InCB').map((e) => ({ ...e, v: e.f[2] })),
  (v) => INCB.indexOf(v), 0);
enums += `enum { ${INCB.map((c) => `INCB_${c}`).join(', ')} };\n`;
h += `static const UniRange uni_incb[] = {\n${incbR.map((r) => `    { ${hex(r.lo)}, ${hex(r.hi)}, INCB_${INCB[r.v]} },`).join('\n')}\n};\n\n`;


// ---- Indic scripts (Devanagari … Malayalam): category and position, as in HarfBuzz ----
const IC = ['X', 'C', 'V', 'N', 'H', 'ZWNJ', 'ZWJ', 'M', 'SM', 'A', 'PLACEHOLDER', 'DOTTEDCIRCLE', 'RS', 'Repha', 'Ra', 'CM', 'Symbol', 'CS'];
const IC_MAP = { Avagraha: 'Symbol', Bindu: 'SM', Brahmi_Joining_Number: 'PLACEHOLDER', Cantillation_Mark: 'A',
  Consonant: 'C', Consonant_Dead: 'C', Consonant_Final: 'CM', Consonant_Head_Letter: 'C', Consonant_Initial_Postfixed: 'C',
  Consonant_Killer: 'M', Consonant_Medial: 'CM', Consonant_Placeholder: 'PLACEHOLDER', Consonant_Preceding_Repha: 'Repha',
  Consonant_Prefixed: 'X', Consonant_Subjoined: 'CM', Consonant_Succeeding_Repha: 'CM', Consonant_With_Stacker: 'CS',
  Gemination_Mark: 'SM', Invisible_Stacker: 'H', Joiner: 'ZWJ', Modifying_Letter: 'X', Non_Joiner: 'ZWNJ', Nukta: 'N',
  Number: 'PLACEHOLDER', Number_Joiner: 'PLACEHOLDER', Pure_Killer: 'M', Register_Shifter: 'RS', Syllable_Modifier: 'SM',
  Tone_Letter: 'X', Tone_Mark: 'N', Virama: 'H', Visarga: 'SM', Vowel: 'V', Vowel_Dependent: 'M', Vowel_Independent: 'V' };
const IC_OVERRIDE = { 0x0930: 'Ra', 0x09B0: 'Ra', 0x09F0: 'Ra', 0x0A30: 'Ra', 0x0AB0: 'Ra', 0x0B30: 'Ra', 0x0BB0: 'Ra',
  0x0C30: 'Ra', 0x0CB0: 'Ra', 0x0D30: 'Ra', 0x0953: 'SM', 0x0954: 'SM', 0x0A72: 'C', 0x0A73: 'C', 0x1CF5: 'C', 0x1CF6: 'C',
  0x0CF1: 'CS', 0x0CF2: 'CS', 0x00A0: 'PLACEHOLDER', 0x00D7: 'PLACEHOLDER', 0x2010: 'PLACEHOLDER', 0x2011: 'PLACEHOLDER',
  0x2012: 'PLACEHOLDER', 0x2013: 'PLACEHOLDER', 0x2014: 'PLACEHOLDER', 0x2212: 'PLACEHOLDER', 0x25CC: 'DOTTEDCIRCLE',
  0x200C: 'ZWNJ', 0x200D: 'ZWJ' };
for (let c = 0x1CE2; c <= 0x1CE8; c++) IC_OVERRIDE[c] = 'A';
IC_OVERRIDE[0x1CED] = 'A'; IC_OVERRIDE[0x1CF8] = 'A'; IC_OVERRIDE[0x1CF9] = 'A';
for (let c = 0xA8F2; c <= 0xA8F7; c++) IC_OVERRIDE[c] = 'Symbol';
const indicRange = (c) => (c >= 0x0900 && c <= 0x0DFF) || (c >= 0x1CD0 && c <= 0x1CFF) || (c >= 0xA8E0 && c <= 0xA8FF) || IC_OVERRIDE[c];
const icVal = new Map();
for (const e of parse(read('IndicSyllabicCategory.txt'), { missing: false }))
  for (let c = e.lo; c <= e.hi; c++) if (indicRange(c)) icVal.set(c, IC.indexOf(IC_MAP[e.v] || 'X'));
for (const [c, v] of Object.entries(IC_OVERRIDE)) icVal.set(Number(c), IC.indexOf(v));
const IP = ['END', 'PRE_C', 'ABOVE_C', 'BELOW_C', 'POST_C', 'AFTER_MAIN', 'PRE_M'];
const IP_MAP = { Not_Applicable: 'END', Left: 'PRE_C', Top: 'ABOVE_C', Bottom: 'BELOW_C', Right: 'POST_C', Bottom_And_Right: 'POST_C',
  Left_And_Right: 'POST_C', Top_And_Bottom: 'BELOW_C', Top_And_Bottom_And_Left: 'BELOW_C', Top_And_Bottom_And_Right: 'POST_C',
  Top_And_Left: 'ABOVE_C', Top_And_Left_And_Right: 'POST_C', Top_And_Right: 'POST_C', Overstruck: 'AFTER_MAIN', Visual_Order_Left: 'PRE_M' };
const ipVal = new Map();
for (const e of parse(read('IndicPositionalCategory.txt'), { missing: false }))
  for (let c = e.lo; c <= e.hi; c++) if (indicRange(c)) ipVal.set(c, IP.indexOf(IP_MAP[e.v] || 'END'));
const toRanges = (m) => {
  const keys = [...m.keys()].sort((a, b) => a - b), out = [];
  for (const c of keys) {
    const v = m.get(c), last = out[out.length - 1];
    if (last && last.hi === c - 1 && last.v === v) last.hi = c; else out.push({ lo: c, hi: c, v });
  }
  return out.filter((r) => r.v !== 0);
};
const icR = toRanges(icVal), ipR = toRanges(ipVal);
enums += `enum { ${IC.map((c) => `IC_${c}`).join(', ')} };\n`;
enums += `enum { ${IP.map((c) => `IP_${c}`).join(', ')} };\n`;
h += `static const UniRange uni_indic_cat[] = {\n${icR.map((r) => `    { ${hex(r.lo)}, ${hex(r.hi)}, IC_${IC[r.v]} },`).join('\n')}\n};\n\n`;
h += `static const UniRange uni_indic_pos[] = {\n${ipR.map((r) => `    { ${hex(r.lo)}, ${hex(r.hi)}, IP_${IP[r.v]} },`).join('\n')}\n};\n\n`;

// two-part canonical (not compatibility) decompositions of the complex scripts
// (Indic, Tibetan, Myanmar, Khmer, Balinese, Brahmi and family…), and which ones are not
// recomposed (क़ = क + ़ stays separate). Sorted by character (binary search).
const complexRange = (c) => (c >= 0x0900 && c <= 0x0DFF) || (c >= 0x0F00 && c <= 0x109F) || (c >= 0x1780 && c <= 0x17FF) ||
  (c >= 0x1B00 && c <= 0x1BFF) || (c >= 0xA800 && c <= 0xABFF) || (c >= 0x11000 && c <= 0x11FFF);
const unicodeData = read('UnicodeData.txt').split('\n').filter(Boolean).map((line) => line.split(';'));
const decomp = [];
for (const f of unicodeData) {
  const c = parseInt(f[0], 16);
  if (!complexRange(c) || !f[5] || f[5].startsWith('<')) continue;
  const parts = f[5].split(' ').map((x) => parseInt(x, 16));
  if (parts.length === 2) decomp.push([c, parts[0], parts[1]]);
}
const excl = new Set();
for (const line of read('CompositionExclusions.txt').split('\n')) {
  const m = line.match(/^([0-9A-F]+)/);
  if (m) excl.add(parseInt(m[1], 16));
}
// those that decompose into something starting with a mark (non-initial) are not composed either
const cccOf = new Map(unicodeData.map((f) => [parseInt(f[0], 16), Number(f[3]) || 0]));
for (const [c, a] of decomp) if (cccOf.get(a)) excl.add(c);
h += `typedef struct { uint32_t cp, a, b; uint8_t excluded; } UniDecomp;\n`;
h += `static const UniDecomp uni_decomp[] = {\n${decomp.map(([c, a, b]) => `    { ${hex(c)}, ${hex(a)}, ${hex(b)}, ${excl.has(c) ? 1 : 0} },`).join('\n')}\n};\n\n`;

// ---- marks (general category Mn, Mc, Me) ----
const gcOf = new Map();
for (const f of unicodeData) gcOf.set(parseInt(f[0], 16), f[2]);
const markMap = new Map();
for (const [c, gc] of gcOf) if (gc === 'Mn' || gc === 'Mc' || gc === 'Me') markMap.set(c, 1);
const markR = toRanges(markMap);
h += `static const UniRange uni_mark[] = {\n${markR.map((r) => `    { ${hex(r.lo)}, ${hex(r.hi)}, 1 },`).join('\n')}\n};\n\n`;

// ---- Khmer and Myanmar: categories like those in HarfBuzz's Indic table (gen-indic-table.py) ----
const SEA = ['X', 'C', 'V', 'N', 'H', 'ZWNJ', 'ZWJ', 'M', 'SM', 'A', 'VD', 'PLACEHOLDER', 'DOTTEDCIRCLE', 'RS', 'MPst',
  'Repha', 'Ra', 'CM', 'Symbol', 'CS', 'SMPst', 'VAbv', 'VBlw', 'VPre', 'VPst', 'Robatic', 'Xgroup', 'Ygroup',
  'As', 'MH', 'MR', 'MW', 'MY', 'PT', 'VS', 'ML'];
const seaBlock = (c) => (c >= 0x1780 && c <= 0x17FF) ? 'Khmer' : (c >= 0x1000 && c <= 0x109F) || (c >= 0xA9E0 && c <= 0xA9FF) ||
  (c >= 0xAA60 && c <= 0xAA7F) || (c >= 0x116D0 && c <= 0x116FF) ? 'Myanmar' :
  c <= 0xFF || (c >= 0x2000 && c <= 0x209F) || (c >= 0x1CD0 && c <= 0x1CFF) || (c >= 0xA8E0 && c <= 0xA8FF) ? 'other' : null;
const iscAll = new Map(), ipcAll = new Map();
for (const e of parse(read('IndicSyllabicCategory.txt'), { missing: false })) for (let c = e.lo; c <= e.hi; c++) iscAll.set(c, e.v);
for (const e of parse(read('IndicPositionalCategory.txt'), { missing: false })) for (let c = e.lo; c <= e.hi; c++) ipcAll.set(c, e.v);
const SEA_OVERRIDE = { 0x2015: 'PLACEHOLDER', 0x2022: 'PLACEHOLDER', 0x25FB: 'PLACEHOLDER', 0x25FC: 'PLACEHOLDER',
  0x25FD: 'PLACEHOLDER', 0x25FE: 'PLACEHOLDER', 0x25CC: 'DOTTEDCIRCLE', 0x00A0: 'PLACEHOLDER',
  0x179A: 'Ra', 0x17CC: 'Robatic', 0x17C9: 'Robatic', 0x17CA: 'Robatic', 0x17C6: 'Xgroup', 0x17CB: 'Xgroup', 0x17CD: 'Xgroup',
  0x17CE: 'Xgroup', 0x17CF: 'Xgroup', 0x17D0: 'Xgroup', 0x17D1: 'Xgroup', 0x17C7: 'Ygroup', 0x17C8: 'Ygroup', 0x17DD: 'Ygroup',
  0x17D3: 'Ygroup', 0x17D9: 'PLACEHOLDER',
  0x104E: 'C', 0x1004: 'Ra', 0x101B: 'Ra', 0x105A: 'Ra', 0x1032: 'A', 0x1036: 'A', 0x103A: 'As', 0x103E: 'MH', 0x1060: 'ML',
  0x103C: 'MR', 0x103D: 'MW', 0x1082: 'MW', 0x103B: 'MY', 0x105E: 'MY', 0x105F: 'MY', 0x1063: 'PT', 0x1064: 'PT', 0x1069: 'PT',
  0x106A: 'PT', 0x106B: 'PT', 0x106C: 'PT', 0x106D: 'PT', 0xAA7B: 'PT', 0x1038: 'SM', 0x1087: 'SM', 0x1088: 'SM', 0x1089: 'SM',
  0x108A: 'SM', 0x108B: 'SM', 0x108C: 'SM', 0x108D: 'SM', 0x108F: 'SM', 0x109A: 'SM', 0x109B: 'SM', 0x109C: 'SM', 0x104A: 'PLACEHOLDER' };
for (let c = 0xFE00; c <= 0xFE0F; c++) SEA_OVERRIDE[c] = 'VS';
const SEA_POS = { Left: 'VPre', Top: 'VAbv', Bottom: 'VBlw', Right: 'VPst', Bottom_And_Right: 'VPst', Left_And_Right: 'VPst',
  Top_And_Bottom: 'VBlw', Top_And_Bottom_And_Left: 'VBlw', Top_And_Bottom_And_Right: 'VPst', Top_And_Left: 'VAbv',
  Top_And_Left_And_Right: 'VPst', Top_And_Right: 'VPst' };
const seaVal = new Map();
for (const c of new Set([...iscAll.keys(), ...Object.keys(SEA_OVERRIDE).map(Number)])) {
  const block = seaBlock(c);
  if (!block && !SEA_OVERRIDE[c]) continue;
  const ipc = ipcAll.get(c) || 'Not_Applicable';
  let cat = IC_MAP[iscAll.get(c)] || 'X';
  if (cat === 'SM' && ipc === 'Not_Applicable') cat = 'SMPst';
  if (SEA_OVERRIDE[c]) cat = SEA_OVERRIDE[c];
  if ((cat === 'M' || cat === 'MPst') && (block === 'Khmer' || block === 'Myanmar')) cat = SEA_POS[ipc] || cat;
  seaVal.set(c, SEA.indexOf(cat));
}
const seaR = toRanges(seaVal);
enums += `enum { ${SEA.map((c) => `SEA_${c}`).join(', ')} };\n#define SEA_NAMES ${SEA.map((c) => `"${c}"`).join(', ')}\n`;
h += `static const UniRange uni_sea[] = {\n${seaR.map((r) => `    { ${hex(r.lo)}, ${hex(r.hi)}, SEA_${SEA[r.v]} },`).join('\n')}\n};\n\n`;

// ---- Universal Shaping Engine: categories as in HarfBuzz's gen-use-table.py ----
// Also needs ArabicShaping.txt and the two USE data files that ship with HarfBuzz
// (src/ms-use/IndicSyllabicCategory-Additional.txt and IndicPositionalCategory-Additional.txt).
const useIsc = new Map(iscAll), useIpc = new Map(ipcAll), ajt = new Map(), udi = new Set();
const inUse = new Set([...iscAll.keys(), ...ipcAll.keys()]);
for (const e of parse(read('ArabicShaping.txt'), { missing: false })) for (let c = e.lo; c <= e.hi; c++) { ajt.set(c, e.f[2]); inUse.add(c); }
for (const e of parse(read('DerivedCoreProperties.txt'), { missing: false }).filter((e) => e.v === 'Default_Ignorable_Code_Point'))
  for (let c = e.lo; c <= e.hi; c++) { udi.add(c); inUse.add(c); }
for (const e of parse(read('IndicSyllabicCategory-Additional.txt'), { missing: false }))
  for (let c = e.lo; c <= e.hi; c++) { useIsc.set(c, e.v === 'Consonant_Final_Modifier' ? 'Syllable_Modifier' : e.v); inUse.add(c); }
for (const e of parse(read('IndicPositionalCategory-Additional.txt'), { missing: false }))
  for (let c = e.lo; c <= e.hi; c++) { useIpc.set(c, e.v === 'NA' ? 'Not_Applicable' : e.v); inUse.add(c); }
const scriptName = new Map();
for (const e of parse(read('Scripts.txt'), { missing: false })) for (let c = e.lo; c <= e.hi; c++) scriptName.set(c, e.v);
const USE_DISABLED = new Set(['Arabic', 'Lao', 'Samaritan', 'Syriac', 'Thai']);
const USE_RULES = [
  ['B', (U, I, D, G, J) => ['Number', 'Consonant', 'Consonant_Head_Letter', 'Tone_Letter', 'Vowel_Independent'].includes(I) ||
    (['C', 'D', 'L', 'R'].includes(J) && I !== 'Joiner') ||
    (G === 'Lo' && ['Avagraha', 'Bindu', 'Consonant_Final', 'Consonant_Medial', 'Consonant_Subjoined', 'Vowel', 'Vowel_Dependent'].includes(I))],
  ['N', (U, I) => I === 'Brahmi_Joining_Number'],
  ['GB', (U, I) => I === 'Consonant_Placeholder' || [0x2015, 0x2022, 0x25FB, 0x25FC, 0x25FD, 0x25FE].includes(U)],
  ['CGJ', (U, I, D, G) => I === 'Joiner' || (D && ['Mc', 'Me', 'Mn'].includes(G))],
  ['F', (U, I, D, G) => (I === 'Consonant_Final' && G !== 'Lo') || I === 'Consonant_Succeeding_Repha'],
  ['FM', (U, I) => I === 'Syllable_Modifier'],
  ['M', (U, I, D, G) => (I === 'Consonant_Medial' && G !== 'Lo') || I === 'Consonant_Initial_Postfixed'],
  ['CM', (U, I) => ['Nukta', 'Gemination_Mark', 'Consonant_Killer'].includes(I)],
  ['SUB', (U, I, D, G) => I === 'Consonant_Subjoined' && G !== 'Lo'],
  ['CS', (U, I) => I === 'Consonant_With_Stacker'],
  ['H', (U, I) => I === 'Virama' && U !== 0x0DCA],
  ['HVM', (U) => U === 0x0DCA],
  ['HN', (U, I) => I === 'Number_Joiner'],
  ['IS', (U, I) => I === 'Invisible_Stacker' && U !== 0x1A60],
  ['G', (U, I) => I === 'Hieroglyph'],
  ['HM', (U, I) => I === 'Hieroglyph_Modifier'],
  ['HR', (U, I) => I === 'Hieroglyph_Mirror'],
  ['J', (U, I) => I === 'Hieroglyph_Joiner'],
  ['SB', (U, I) => I === 'Hieroglyph_Mark_Begin' || I === 'Hieroglyph_Segment_Begin'],
  ['SE', (U, I) => I === 'Hieroglyph_Mark_End' || I === 'Hieroglyph_Segment_End'],
  ['ZWNJ', (U, I) => I === 'Non_Joiner'],
  ['O', null],                                  // below: depends on the others
  ['RK', (U, I) => I === 'Reordering_Killer'],
  ['R', (U, I) => I === 'Consonant_Preceding_Repha' || I === 'Consonant_Prefixed'],
  ['Sk', (U) => U === 0x1A60],
  ['SM', (U, I) => I === 'Symbol_Modifier'],
  ['V', (U, I, D, G) => I === 'Pure_Killer' || (G !== 'Lo' && (I === 'Vowel' || I === 'Vowel_Dependent'))],
  ['VM', (U, I, D, G) => ['Tone_Mark', 'Cantillation_Mark', 'Register_Shifter', 'Visarga'].includes(I) || (G !== 'Lo' && I === 'Bindu')],
  ['WJ', null],
];
const useRule = Object.fromEntries(USE_RULES);
useRule.WJ = (U, I, D, G) => (D && ![0x115F, 0x1160, 0x3164, 0xFFA0, 0x1BCA0, 0x1BCA1, 0x1BCA2, 0x1BCA3].includes(U) && I === 'Other' &&
  !useRule.CGJ(U, I, D, G)) || G === 'Cn';
useRule.O = (U, I, D, G, J) => (G === 'Po' || ['Consonant_Dead', 'Joiner', 'Modifying_Letter', 'Other'].includes(I)) &&
  !useRule.B(U, I, D, G, J) && !useRule.GB(U, I, D, G, J) && !useRule.CGJ(U, I, D, G, J) && !useRule.SM(U, I, D, G, J) && !useRule.WJ(U, I, D, G, J);
const USE_POS = {
  F: { Abv: ['Top'], Blw: ['Bottom'], Pst: ['Right'] },
  M: { Abv: ['Top'], Blw: ['Bottom', 'Bottom_And_Left', 'Bottom_And_Right'], Pst: ['Right'], Pre: ['Left', 'Top_And_Bottom_And_Left'] },
  CM: { Abv: ['Top'], Blw: ['Bottom', 'Overstruck'] },
  V: { Abv: ['Top', 'Top_And_Bottom', 'Top_And_Bottom_And_Right', 'Top_And_Right'], Blw: ['Bottom', 'Overstruck', 'Bottom_And_Right'],
    Pst: ['Right'], Pre: ['Left', 'Top_And_Left', 'Top_And_Left_And_Right', 'Left_And_Right'] },
  VM: { Abv: ['Top'], Blw: ['Bottom', 'Overstruck'], Pst: ['Right'], Pre: ['Left'] },
  SM: { Abv: ['Top'], Blw: ['Bottom'] },
  FM: { Abv: ['Top'], Blw: ['Bottom'], Pst: ['Not_Applicable'] },
};
const USE = ['O', 'B', 'N', 'GB', 'CGJ', 'SUB', 'H', 'HN', 'ZWNJ', 'WJ', 'R', 'CS', 'IS', 'Sk', 'G', 'J', 'SB', 'SE', 'HVM', 'HM', 'HR', 'RK',
  'FAbv', 'FBlw', 'FPst', 'MAbv', 'MBlw', 'MPst', 'MPre', 'CMAbv', 'CMBlw', 'VAbv', 'VBlw', 'VPst', 'VPre', 'VMAbv', 'VMBlw', 'VMPst',
  'VMPre', 'SMAbv', 'SMBlw', 'FMAbv', 'FMBlw', 'FMPst'];
const useVal = new Map();
let useConflicts = 0;
for (const U of inUse) {
  const sc = scriptName.get(U) || 'Unknown';
  if (USE_DISABLED.has(sc)) continue;
  const G = gcOf.get(U) || 'Cn';
  if (G === 'Cn') continue;                     // unassigned in this Unicode version
  let I = useIsc.get(U) || 'Other', P = useIpc.get(U) || 'Not_Applicable';
  const D = udi.has(U), J = ajt.get(U) || 'X';
  if (U >= 0x1CE2 && U <= 0x1CE8) I = 'Cantillation_Mark';
  if ((U >= 0x0F18 && U <= 0x0F19) || (U >= 0x0F3E && U <= 0x0F3F)) I = 'Vowel_Dependent';
  if (U === 0x1CED) I = 'Tone_Mark';
  const hits = USE_RULES.map(([k]) => k).filter((k) => useRule[k](U, I, D, G, J));
  if (hits.length !== 1) { useConflicts++; if (!hits.length) continue; }
  let cat = hits[0];
  if ([0x11302, 0x11303, 0x114C1].includes(U)) P = 'Top';
  const pm = USE_POS[cat];
  if (pm) {
    const suf = Object.keys(pm).filter((k) => pm[k].includes(P));
    if (suf.length !== 1) { useConflicts++; continue; }
    cat += suf[0];
  }
  if (cat !== 'O') useVal.set(U, USE.indexOf(cat));
}
const useR = toRanges(useVal);
enums += `enum { ${USE.map((c) => `USE_${c}`).join(', ')}, USE_COUNT };\n#define USE_NAMES ${USE.map((c) => `"${c}"`).join(', ')}\n`;
h += `static const UniRange uni_use[] = {\n${useR.map((r) => `    { ${hex(r.lo)}, ${hex(r.hi)}, USE_${USE[r.v]} },`).join('\n')}\n};\n\n`;

h = `/* unidata.h — generated by tools/gen-unicode.mjs from Unicode ${version}. Do not edit.
 * Sorted range tables; absent values are the default value (0). The tables
 * are only compiled in unicode.c; everything else uses the functions in unicode.h. */
#ifndef LORD_UNIDATA_H
#define LORD_UNIDATA_H

#include <stdint.h>

#define UNIDATA_VERSION "${version}"

${enums}
#ifdef UNIDATA_TABLES

typedef struct { uint32_t lo, hi; uint8_t v; } UniRange;
typedef struct { uint32_t a, b; } UniPair;
typedef struct { uint32_t a, b; uint8_t open; } UniBracket;

` + h + `/* Value of cp in a range table (0 if absent). */
static inline uint8_t uni_lookup(const UniRange *t, int n, uint32_t cp) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < t[mid].lo) hi = mid - 1;
        else if (cp > t[mid].hi) lo = mid + 1;
        else return t[mid].v;
    }
    return 0;
}
#define UNI_N(t) ((int)(sizeof t / sizeof *t))

#endif /* UNIDATA_TABLES */
#endif
`;
const out = path.join(root, 'server/src/ui/unidata.h');
fs.writeFileSync(out, h);
console.log(`${out}: Unicode ${version}, Indic ${icR.length}/${ipR.length}/${decomp.length}, Khmer/Myanmar ${seaR.length}, USE ${useR.length} (${useConflicts} without a unique category), marks ${markR.length}, bidi ${bidi.length} ranges, ccc ${ccc.length}, invisibles ${ign.length}, graphemes ${gcb.length}/${extpict.length}/${incbR.length}, joining ${join.length}, scripts ${scripts.length}/${script.length}, line break ${lb.length}, mirrors ${mirror.length}, brackets ${brackets.length}`);
