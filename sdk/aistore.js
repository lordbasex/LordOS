// aistore.js — Settings → Artificial intelligence (0.8.9.6.2): a store around Lumo, the pure part (apps/settings/AI.jsx,
// docs/AI.md; tested by server/tests/ai_store_test.mjs).
//
// Lumo is the sum of five pieces: the Ear (speech to text), the Brain (the language model), the Voice (reading aloud),
// the Eyes (reading text in pictures) and the Hands (doing things in LordOS with your permission). Each piece is a
// capability with its models, shown as the wallpaper gallery shows its collections: a card per model with its size,
// memory, license and state (installed ✓, downloadable ☁, coming soon), filters with counts and a search.
//
// One brain, many functions: the language model is one; the functions are groups of LordOS tools that brain uses, each
// with the level it needs (read only, with your permission, administrator). Today only Sentinela works.
//
// What exists inside LordOS today and what does not (no new helper, dependency or network path comes with this page):
//   brain  the lordos-llm-* packages and the llama.cpp engine (sdk/llmcatalog.js), installed and chosen
//   voice  the lordos-voice-* packages (sdk/voicecatalog.js), per language
//   eye    Tesseract (the confined lord-ocr helper) and Debian's tesseract-ocr-<lang> language data
//   ear    Whisper (whisper.cpp): coming soon, its facts only
//   img    Real-ESRGAN: coming soon, its facts only (it made the 4K wallpapers, off LordOS)
//   hands  the agent's tools (Hands, docs/HANDS.md): lordos-mcp and org.lordos.Hands1, for Claude Code today and Lumo's
//          own agent later; always there, on only when the person turns it on for a session

import { VOICES, SOURCE_LICENSES, hasVoice } from './voicecatalog.js';

export const CAPS = ['brain', 'ear', 'voice', 'eye', 'img'];
// each capability's colour (the mockup's tokens) and its icon in assets/ai/icons/ (<name>-96.png, white)
export const CAP_COLOR = { brain: '#e04f86', ear: '#2bb3c0', voice: '#7c5cff', eye: '#f2994a', img: '#3ccf8e' };
export const CAP_ICON = { brain: 'brain', ear: 'ear', voice: 'voice', eye: 'eye', img: 'image', hands: 'hand' };
// capabilities with nothing inside LordOS yet
export const CAP_SOON = new Set(['ear', 'img']);

// Lumo's pieces, in the order of a conversation; cap: the capability that makes the piece (hands: none, it is part of LordOS)
export const PIECES = [
  { id: 'ear', cap: 'ear' }, { id: 'brain', cap: 'brain' }, { id: 'voice', cap: 'voice' }, { id: 'eyes', cap: 'eye' }, { id: 'hands', cap: null },
];

// What Lumo can do: one brain, many functions (groups of LordOS tools, MCP later). level: 0 read only, 1 with your
// permission, 2 administrator (the password always in the system's dialog, never in a chat). Sentinela and Writing
// (✨ AI on selected text, 0.8.9.6.2; docs/AI.md "Writing") work.
export const LEVELS = ['read', 'ask', 'admin'];
export const FUNCTIONS = [
  { id: 'writing', level: 0, ready: true, llm: true }, { id: 'sentinela', level: 0, ready: true, llm: true },
  { id: 'hands', level: 1, ready: true },
  { id: 'notes', level: 1 }, { id: 'programs', level: 1 }, { id: 'logs', level: 0 },
  { id: 'files', level: 0 }, { id: 'apps', level: 1 }, { id: 'machines', level: 1 }, { id: 'settings', level: 1 }, { id: 'system', level: 2 },
];

// ---- the models that are not inside LordOS yet: their real facts, no install ----

// whisper.cpp's ggml models (huggingface.co/ggerganov/whisper.cpp), MIT; memory as whisper.cpp's README lists it
export const EAR_MODELS = [
  { id: 'whisper-base', name: 'Whisper Base', bytes: 147951465, ramMb: 388, license: 'MIT', source: 'ggerganov/whisper.cpp · ggml-base.bin' },
  { id: 'whisper-small', name: 'Whisper Small', bytes: 487601967, ramMb: 852, license: 'MIT', source: 'ggerganov/whisper.cpp · ggml-small.bin', recommended: true },
  { id: 'whisper-turbo', name: 'Whisper Large v3 Turbo', bytes: 574041195, ramMb: 0, license: 'MIT', source: 'ggerganov/whisper.cpp · ggml-large-v3-turbo-q5_0.bin' },
];
// Real-ESRGAN (github.com/xinntao/Real-ESRGAN, BSD-3-Clause); the x4 model made LordOS's 4K wallpapers
export const IMAGE_MODELS = [
  { id: 'esrgan-x4', name: 'Real-ESRGAN x4', bytes: 67040989, ramMb: 0, license: 'BSD-3-Clause', source: 'xinntao/Real-ESRGAN · RealESRGAN_x4plus', recommended: true, compare: true },
  { id: 'esrgan-x4-anime', name: 'Real-ESRGAN x4 Anime', bytes: 17938799, ramMb: 0, license: 'BSD-3-Clause', source: 'xinntao/Real-ESRGAN · RealESRGAN_x4plus_anime_6B', compare: true },
];

// ---- Vision: Tesseract's language data, one Debian package per LordOS interface language ----

export const TESSDATA = '/usr/share/tesseract-ocr/5/tessdata';
// [LordOS locale, Tesseract's data name, Debian 13's installed size in KiB]
export const OCR_LANGS = [
  ['en', 'eng', 4032], ['es', 'spa', 2256], ['pt', 'por', 1952], ['fr', 'fra', 1119], ['it', 'ita', 2654], ['de', 'deu', 1505],
  ['ar', 'ara', 1414], ['az', 'aze', 3458], ['bg', 'bul', 1651], ['bn', 'ben', 851], ['ca', 'cat', 1135], ['el', 'ell', 1402],
  ['fa', 'fas', 437], ['fi', 'fin', 7697], ['fil', 'fil', 1814], ['hi', 'hin', 1112], ['hu', 'hun', 5188], ['id', 'ind', 1112],
  ['ja', 'jpn', 2429], ['ko', 'kor', 1654], ['lt', 'lit', 3096], ['ms', 'msa', 1722], ['nl', 'nld', 5924], ['pl', 'pol', 4669],
  ['ro', 'ron', 2336], ['ru', 'rus', 3787], ['sv', 'swe', 4085], ['sw', 'swa', 2132], ['th', 'tha', 1063], ['tr', 'tur', 4459],
  ['uk', 'ukr', 3751], ['ur', 'urd', 1381], ['vi', 'vie', 534], ['zh-Hans', 'chi_sim', 2427], ['zh-Hant', 'chi_tra', 2327],
].map(([locale, code, kib]) => ({ locale, code, bytes: kib * 1024, package: `tesseract-ocr-${code.replace(/_/g, '-')}` }));
// English is what lord-ocr needs (the lordos package depends on it): never offered for removal
export const OCR_BASE = 'eng';
// a package Settings may ask pkg-install / pkg-remove for: one of these, nothing else
export const ocrPackageOk = (name) => OCR_LANGS.some((l) => l.package === name);

// the data files in tessdata → the codes installed ('eng.traineddata' → 'eng'); osd is not a language
export const tessCodes = (names) => [...new Set((names || []).map((n) => /^([a-z_]{3,16})\.traineddata$/.exec(String(n))).filter(Boolean)
  .map((m) => m[1]).filter((c) => c !== 'osd'))];

// The languages Tesseract reads with: the interface's first (it wins where two read a word differently), then English,
// then the others installed, as many as lord-ocr takes (64 characters).
export function ocrLangs(uiLocale, installed) {
  const have = new Set(installed || []);
  const mine = OCR_LANGS.find((l) => l.locale === uiLocale || l.locale.split('-')[0] === String(uiLocale || '').split(/[-_]/)[0]);
  const order = [mine && mine.code, OCR_BASE, ...OCR_LANGS.map((l) => l.code)].filter((c) => c && have.has(c));
  let out = '';
  for (const c of [...new Set(order)]) { const next = out ? `${out}+${c}` : c; if (next.length > 64) break; out = next; }
  return out;
}

// ---- the entries: one per model, the same shape for every capability ----
// { id, cap, kind, name, bytes, ramMb, license, source, state: 'installed' | 'cloud' | 'soon', recommended, inUse, … }
//   kind  'llm' | 'engine' | 'voiceLang' | 'ocr' | 'ocrLang' | 'soon'
//   name  a proper name, or null when the page names it (a language)

// models: LLM_MODELS (sdk/llmcatalog.js); eng: { available, models: [{ id }] } (sentinelaEngine); chosen: the model in use
export function brainEntries(models, eng, chosen) {
  const have = new Set(((eng && eng.models) || []).map((m) => m.id));
  return [
    ...(models || []).map((m) => ({
      id: `llm:${m.id}`, cap: 'brain', kind: 'llm', name: m.name, bytes: m.bytes, ramMb: m.ramMb, license: m.license, source: m.source,
      state: have.has(m.id) ? 'installed' : 'cloud', recommended: !!m.recommended, inUse: have.has(m.id) && chosen === m.id, model: m,
    })),
    { id: 'llm:engine', cap: 'brain', kind: 'engine', name: 'llama.cpp', bytes: 0, ramMb: 0, license: 'MIT', source: 'ggml-org/llama.cpp',
      state: eng && eng.available ? 'installed' : 'cloud' },
  ];
}

// langs: VOICE_LANGS (sdk/voicecatalog.js); scan: scanVoices(); one entry per language: its model and its own voice
export function voiceEntries(langs, scan, chosenVoices = {}) {
  return (langs || []).map((L) => {
    const s = (scan && scan[L.id]) || {};
    return {
      id: `voice:${L.id}`, cap: 'voice', kind: 'voiceLang', name: null, lang: L.id, voice: chosenVoices[L.id] || L.own,
      bytes: (L.mb.standard + L.mb.voice) * 1e6, bytesHd: (L.mb.hd + L.mb.voiceHd) * 1e6, ramMb: 0, license: 'CC-BY-4.0', source: 'kyutai/pocket-tts',
      state: s.standard || s.hd ? 'installed' : 'cloud', hd: !!s.hd, recommended: false,
    };
  });
}

// The Voice page's cards for one language (L: a VOICE_LANGS entry; st: scanVoices()[L.id]): its standard and HD models,
// the voices of the quality reading uses (HD unless standard is asked for or only standard is installed: say.c's rule),
// and the person's own voices (names). chosen: the voice used for that language.
export function voicePage(L, st, quality, chosen, own = []) {
  const s = st || { standard: false, hd: false, voices: { standard: new Set(), hd: new Set() } };
  const hd = quality === 'hd' || (quality === 'auto' && (s.hd || !s.standard));
  const models = ['standard', 'hd'].map((q) => ({
    id: `vmodel:${L.id}:${q}`, cap: 'voice', kind: 'voiceModel', q, lang: L.id, bytes: L.mb[q] * 1e6, license: 'CC-BY-4.0', source: 'kyutai/pocket-tts',
    state: (q === 'hd' ? s.hd : s.standard) ? 'installed' : 'cloud', recommended: q === 'standard',
  }));
  const voices = VOICES.filter((x) => hasVoice(x.id, hd, L.id)).map((x) => ({
    id: `vvoice:${L.id}:${x.id}:${hd ? 'hd' : 'standard'}`, cap: 'voice', kind: 'voiceVoice', lang: L.id, voice: x.id, gender: x.gender, hd,
    bytes: L.mb[hd ? 'voiceHd' : 'voice'] * 1e6, license: SOURCE_LICENSES[x.source] || '', source: x.source,
    state: s.voices[hd ? 'hd' : 'standard'].has(x.id) ? 'installed' : 'cloud', inUse: chosen === x.id, recommended: x.id === L.own,
  }));
  const mine = (own || []).map((name) => ({ id: `own:${name}`, cap: 'voice', kind: 'ownVoice', name, voice: name, lang: L.id, state: 'installed', inUse: chosen === name, bytes: 0 }));
  return { hd, models, voices, own: mine };
}

// ocrReady: Tesseract and its English data are here; installed: the codes in tessdata
export function eyeEntries(ocrReady, installed) {
  const have = new Set(installed || []);
  return [
    { id: 'ocr:tesseract', cap: 'eye', kind: 'ocr', name: 'Tesseract OCR', bytes: OCR_LANGS.find((l) => l.code === OCR_BASE).bytes, ramMb: 0,
      license: 'Apache-2.0', source: 'tesseract-ocr (Debian 13)', state: ocrReady ? 'installed' : 'cloud', recommended: true },
    ...OCR_LANGS.filter((l) => l.code !== OCR_BASE).map((l) => ({
      id: `ocr:${l.code}`, cap: 'eye', kind: 'ocrLang', name: null, locale: l.locale, code: l.code, package: l.package, bytes: l.bytes, ramMb: 0,
      license: 'Apache-2.0', source: l.package, state: have.has(l.code) ? 'installed' : 'cloud',
    })),
  ];
}

const soonEntry = (cap) => (m) => ({ id: `${cap}:${m.id}`, cap, kind: 'soon', name: m.name, bytes: m.bytes, ramMb: m.ramMb, license: m.license,
  source: m.source, state: 'soon', recommended: !!m.recommended, compare: !!m.compare });
export const earEntries = () => EAR_MODELS.map(soonEntry('ear'));
export const imageEntries = () => IMAGE_MODELS.map(soonEntry('img'));

// ---- Lumo's pieces ----

// entries: every entry; a piece is ready when a model of its capability is installed (the brain: a model and the
// engine); the Hands are part of LordOS (docs/HANDS.md); the Ear doesn't exist yet
export function pieceReady(id, entries) {
  const of = (cap) => (entries || []).filter((e) => e.cap === cap);
  if (id === 'hands') return true;
  if (id === 'ear') return false;
  if (id === 'brain') return of('brain').some((e) => e.kind === 'llm' && e.state === 'installed') && of('brain').some((e) => e.kind === 'engine' && e.state === 'installed');
  const cap = PIECES.find((p) => p.id === id)?.cap;
  return !!cap && of(cap).some((e) => e.state === 'installed');
}
export function lumoState(entries) {
  const pieces = PIECES.map((p) => ({ ...p, ready: pieceReady(p.id, entries), soon: p.id === 'ear' }));
  const ready = pieces.filter((p) => p.ready).length;
  // waving when every piece that exists today is installed; asleep while one you could download is missing
  const missing = pieces.filter((p) => !p.ready && !p.soon).map((p) => p.id);
  // talking needs the Ear, the Brain, the Voice and the Hands: the Ear isn't there yet
  const canTalk = pieces.filter((p) => ['ear', 'brain', 'voice', 'hands'].includes(p.id)).every((p) => p.ready);
  return { pieces, ready, total: pieces.length, missing, pose: missing.length ? 'sleep' : 'wave', canTalk };
}

// ---- search and filters ----

export const AI_FILTERS = ['all', 'installed', ...CAPS];
export const foldText = (s) => String(s || '').normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();
// every word of the query is in the entry's name (as shown), its id, its source, its capability's id or name
export function matchesQuery(e, query, label = (x) => x.name || '', capName = (c) => c) {
  const words = foldText(query).split(/\s+/).filter(Boolean);
  if (!words.length) return true;
  const hay = foldText([label(e), e.id, e.source, e.cap, capName(e.cap)].join(' '));
  return words.every((w) => hay.includes(w));
}
// The functions ("What Lumo can do") a search finds, by their name and description in the person's language (and
// their id): Sentinela, Writing… name(f), desc(f) → text. Every word of the query must be in one of them.
// The functions that use the language model (the brain's sheet, "Used by"), from FUNCTIONS: name(f) → text
export const llmFunctions = (list = FUNCTIONS) => (list || []).filter((f) => f.ready && f.llm);
// Whether a model's sheet can try it: that model installed, the engine there, and a function that uses it on (Sentinela
// or Writing, which is on with a model unless the person turned it off); not Sentinela in particular
export const llmTryReady = ({ installed, engine, sentinelaOn, writingChoice }) => !!installed && !!engine && (!!sentinelaOn || writingChoice !== false);

export function matchFunctions(list, query, name = (f) => f.id, desc = () => '') {
  const words = foldText(query).split(/\s+/).filter(Boolean);
  if (!words.length) return [];
  return (list || []).filter((f) => { const hay = foldText([name(f), desc(f), f.id].join(' ')); return words.every((w) => hay.includes(w)); });
}
export const matchesFilter = (e, f) => f === 'all' || (f === 'installed' ? e.state === 'installed' : e.cap === f);
export const filterEntries = (list, { filter = 'all', query = '', label, capName } = {}) =>
  (list || []).filter((e) => matchesFilter(e, filter) && matchesQuery(e, query, label, capName));
export function filterCounts(list, query = '', label, capName) {
  const out = {};
  for (const f of AI_FILTERS) out[f] = filterEntries(list, { filter: f, query, label, capName }).length;
  return out;
}
// [cap, entries] in CAPS order, the empty ones left out
export const groupByCap = (list) => CAPS.map((c) => [c, (list || []).filter((e) => e.cap === c)]).filter(([, l]) => l.length);
// a capability's card: how many models, how many installed, coming soon when none can be installed
export function capCard(cap, list) {
  const mine = (list || []).filter((e) => e.cap === cap);
  return { cap, count: mine.length, installed: mine.filter((e) => e.state === 'installed').length, soon: CAP_SOON.has(cap) || (mine.length > 0 && mine.every((e) => e.state === 'soon')) };
}

// ---- space ----

// the bytes the models take, from what the disk says (each folder's du); a missing or unmeasured one counts 0
export const modelBytes = (sizes) => Object.values(sizes || {}).reduce((a, n) => a + (Number.isFinite(n) && n > 0 ? n : 0), 0);
// the cards a capability page shows first: recommended, installed, then the rest, each in catalog order
export function sortForPage(list) {
  const rank = (e) => (e.inUse ? 0 : e.recommended ? 1 : e.state === 'installed' ? 2 : 3);
  return (list || []).map((e, i) => [e, i]).sort((a, b) => rank(a[0]) - rank(b[0]) || a[1] - b[1]).map(([e]) => e);
}

// ---- (0.8.9.6.2) where the Brain computes (Settings → AI, "Where they run"; docs/AI.md "The Brain on the GPU") ----

// the person's choice, settings.brain.backend: 'auto' (CUDA, else Vulkan, else the processor), 'cpu' or 'gpu'
export const BRAIN_BACKENDS = ['auto', 'cpu', 'gpu'];
export const brainBackend = (s) => {
  const b = s && s.brain && s.brain.backend;
  return BRAIN_BACKENDS.includes(b) ? b : 'auto';
};

const API_NAME = { vulkan: 'Vulkan', cuda: 'CUDA' };
// The line under the choice, from the engine's state (lord.sentinela.state(), engine.h): { key, vars } of
// settings.aiStore.foot.*. What the engine used last this session when it has run; before that, what it will use.
export function brainWhere(state, mode) {
  const st = state || {};
  const gpus = st.gpus || {};
  const ready = gpus.cuda ? 'cuda' : gpus.vulkan ? 'vulkan' : null;
  if (st.backend === 'vulkan' || st.backend === 'cuda') {
    const vars = { api: API_NAME[st.backend], device: typeof st.device === 'string' && st.device ? st.device : API_NAME[st.backend] };
    const total = Number.isInteger(st.layersTotal) && st.layersTotal > 0 ? st.layersTotal : 0;
    const layers = Number.isInteger(st.layers) && st.layers >= 0 && st.layers <= total ? st.layers : 0;
    if (total && layers < total) return { key: 'brainGpuPart', vars: { ...vars, layers, total } };
    return { key: 'brainGpu', vars };
  }
  if (mode === 'cpu') return { key: 'brainCpuChosen', vars: {} };
  if (st.backend === 'cpu') {
    const why = { start: 'brainCpuStart', memory: 'brainCpuMemory', untested: 'brainCpuUntested', none: 'brainCpuNoGpu' }[st.fallback];
    return { key: why || 'brainCpu', vars: {} };
  }
  if (ready) return { key: 'brainGpuReady', vars: { api: API_NAME[ready] } };
  return { key: 'brainCpuNoGpu', vars: {} };
}
