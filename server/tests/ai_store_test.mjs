// ai_store_test.mjs — Settings → Artificial intelligence (sdk/aistore.js): the entries of each capability from what is
// installed, Lumo's pieces and pose, the search and its filters with counts, the capability cards, the Voice page's
// cards, Tesseract's languages and packages, and the space the models take. node server/tests/ai_store_test.mjs
import {
  CAPS, PIECES, FUNCTIONS, LEVELS, EAR_MODELS, IMAGE_MODELS, OCR_LANGS, OCR_BASE, AI_FILTERS, ocrPackageOk, tessCodes, ocrLangs, brainEntries,
  voiceEntries, voicePage, eyeEntries, earEntries, imageEntries, pieceReady, lumoState, matchesQuery, filterEntries, filterCounts, groupByCap,
  capCard, modelBytes, sortForPage, CAP_COLOR, CAP_ICON, matchFunctions, llmFunctions, llmTryReady, BRAIN_BACKENDS, brainBackend, brainWhere,
} from '../../sdk/aistore.js';
import { LLM_MODELS } from '../../sdk/llmcatalog.js';
import { VOICE_LANGS } from '../../sdk/voicecatalog.js';
import { readdirSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

let ok = 0, bad = 0;
const eq = (name, a, b) => {
  if (JSON.stringify(a) === JSON.stringify(b)) ok++;
  else { bad++; console.error(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); }
};
const yes = (name, v) => eq(name, !!v, true);

const emptyScan = () => Object.fromEntries(VOICE_LANGS.map((L) => [L.id, { standard: false, hd: false, voices: { standard: new Set(), hd: new Set() } }]));

// ---- the catalog ----
eq('five capabilities', CAPS, ['brain', 'ear', 'voice', 'eye', 'img']);
eq('five pieces in a conversation\'s order', PIECES.map((p) => p.id), ['ear', 'brain', 'voice', 'eyes', 'hands']);
yes('every capability has a colour and an icon', CAPS.every((c) => CAP_COLOR[c] && CAP_ICON[c]));
const root = fileURLToPath(new URL('../../', import.meta.url));
yes('every icon is in assets/ai/icons', Object.values(CAP_ICON).every((n) => existsSync(`${root}assets/ai/icons/${n}-symbolic.svg`) && existsSync(`${root}assets/ai/icons/${n}-96.png`)));
yes('the before-and-after picture is shipped', existsSync(`${root}assets/ai/esrgan-compare.jpg`));
eq('ten functions, the two that work first', FUNCTIONS.map((f) => f.id), ['writing', 'sentinela', 'notes', 'programs', 'logs', 'files', 'apps', 'machines', 'settings', 'system']);
eq('Writing and Sentinela work today', FUNCTIONS.filter((f) => f.ready).map((f) => f.id), ['writing', 'sentinela']);
eq('Writing only reads (the text comes back on the person\'s click)', FUNCTIONS.find((f) => f.id === 'writing').level, 0);
yes('every function has a level', FUNCTIONS.every((f) => LEVELS[f.level]));
eq('System needs an administrator', LEVELS[FUNCTIONS.find((f) => f.id === 'system').level], 'admin');
eq('Sentinela only reads', LEVELS[FUNCTIONS.find((f) => f.id === 'sentinela').level], 'read');
eq('three Whisper models', EAR_MODELS.map((m) => m.id), ['whisper-base', 'whisper-small', 'whisper-turbo']);
eq('ggml-base is 148 MB', Math.round(EAR_MODELS[0].bytes / 1e6), 148);
eq('Real-ESRGAN x4 ~64 MB, anime ~17 MB', IMAGE_MODELS.map((m) => Math.round(m.bytes / 1048576)), [64, 17]);
eq('35 OCR languages: one per interface language', OCR_LANGS.length, 35);
const locales = readdirSync(`${root}assets/locales`).filter((d) => existsSync(`${root}assets/locales/${d}/meta.json`));
eq('OCR languages cover the interface languages', OCR_LANGS.map((l) => l.locale).sort(), locales.sort());
eq('Chinese is chi_sim → tesseract-ocr-chi-sim', OCR_LANGS.find((l) => l.locale === 'zh-Hans').package, 'tesseract-ocr-chi-sim');
yes('a Tesseract package is accepted', ocrPackageOk('tesseract-ocr-ara'));
yes('another package is refused', !ocrPackageOk('tesseract-ocr-all') && !ocrPackageOk('bash') && !ocrPackageOk('tesseract-ocr-ara; rm'));

// ---- Tesseract's data ----
eq('tessdata names → codes, osd left out', tessCodes(['eng.traineddata', 'osd.traineddata', 'spa.traineddata', 'pdf.ttf', 'chi_sim.traineddata', 'configs']), ['eng', 'spa', 'chi_sim']);
eq('the interface language first, then English', ocrLangs('es', ['eng', 'spa', 'por']), 'spa+eng+por');
eq('pt-BR is Portuguese', ocrLangs('pt-BR', ['por', 'eng']), 'por+eng');
eq('a language not installed is skipped', ocrLangs('ar', ['eng']), 'eng');
eq('Chinese (Simplified)', ocrLangs('zh-Hans', ['chi_sim', 'eng']), 'chi_sim+eng');
yes('at most 64 characters (lord-ocr)', ocrLangs('en', OCR_LANGS.map((l) => l.code)).length <= 64);
eq('nothing installed: nothing', ocrLangs('en', []), '');

// ---- the entries ----
const brainNone = brainEntries(LLM_MODELS, { available: false, models: [] }, null);
eq('brain: two models and the engine', brainNone.map((e) => e.id), ['llm:qwen3-1.7b', 'llm:qwen3-4b', 'llm:engine']);
eq('brain: none installed', brainNone.map((e) => e.state), ['cloud', 'cloud', 'cloud']);
eq('brain: 1.7B recommended', brainNone.filter((e) => e.recommended).map((e) => e.id), ['llm:qwen3-1.7b']);
const brainOne = brainEntries(LLM_MODELS, { available: true, models: [{ id: 'qwen3-1.7b', bytes: 1 }] }, 'qwen3-1.7b');
eq('brain: installed and in use', brainOne.map((e) => [e.state, !!e.inUse]), [['installed', true], ['cloud', false], ['installed', false]]);
eq('brain: sizes from the catalog', brainOne[0].bytes, LLM_MODELS[0].bytes);

const scan = emptyScan();
scan.es.standard = true; scan.es.voices.standard.add('lola');
const voices = voiceEntries(VOICE_LANGS, scan, { es: 'alba' });
eq('voice: one per language', voices.length, VOICE_LANGS.length);
eq('voice: Spanish installed, others not', voices.map((e) => e.state).filter((s) => s === 'installed').length, 1);
eq('voice: the voice chosen for the language', voices[0].voice, 'alba');
eq('voice: otherwise the language\'s own', voices.find((e) => e.lang === 'pt').voice, 'rafael');
eq('voice: standard size = model + voice', voices[0].bytes, (128 + 7) * 1e6);

const vp = voicePage(VOICE_LANGS[0], scan.es, 'auto', 'lola', ['Mine']);
eq('voice page: standard installed → standard voices', vp.hd, false);
eq('voice page: two models', vp.models.map((e) => [e.q, e.state]), [['standard', 'installed'], ['hd', 'cloud']]);
yes('voice page: Lola installed and in use', vp.voices.some((e) => e.voice === 'lola' && e.state === 'installed' && e.inUse));
yes('voice page: Daan is HD only in Spanish', !vp.voices.some((e) => e.voice === 'daan'));
eq('voice page: own voices', vp.own.map((e) => [e.kind, e.voice, e.state]), [['ownVoice', 'Mine', 'installed']]);
eq('voice page: nothing installed → HD voices (as say.c)', voicePage(VOICE_LANGS[0], emptyScan().es, 'auto', 'lola').hd, true);
eq('voice page: standard asked for', voicePage(VOICE_LANGS[0], emptyScan().es, 'standard', 'lola').hd, false);
eq('voice page: a missing scan is empty', voicePage(VOICE_LANGS[0], undefined, 'auto', 'lola').models.map((e) => e.state), ['cloud', 'cloud']);

const eyes = eyeEntries(true, ['eng', 'spa', 'chi_sim']);
eq('eye: Tesseract and 34 more languages', eyes.length, 35);
eq('eye: Tesseract installed', eyes[0].state, 'installed');
eq('eye: English is Tesseract itself, not a card', eyes.filter((e) => e.code === 'eng').length, 0);
eq('eye: Spanish and Chinese installed', eyes.filter((e) => e.state === 'installed' && e.kind === 'ocrLang').map((e) => e.code), ['spa', 'chi_sim']);
eq('eye: no Tesseract', eyeEntries(false, [])[0].state, 'cloud');
eq('ear: coming soon', earEntries().map((e) => e.state), ['soon', 'soon', 'soon']);
eq('img: coming soon, with a comparison', imageEntries().map((e) => [e.state, e.compare]), [['soon', true], ['soon', true]]);

// ---- Lumo ----
const all = (b = brainNone, v = voiceEntries(VOICE_LANGS, emptyScan()), e = eyeEntries(false, [])) => [...b, ...earEntries(), ...v, ...e, ...imageEntries()];
eq('nothing installed: no piece', lumoState(all()).ready, 0);
eq('nothing installed: asleep', lumoState(all()).pose, 'sleep');
eq('nothing installed: what to download', lumoState(all()).missing, ['brain', 'voice', 'eyes']);
yes('the brain needs a model and the engine', !pieceReady('brain', all(brainEntries(LLM_MODELS, { available: false, models: [{ id: 'qwen3-1.7b' }] }, null))));
yes('… both installed', pieceReady('brain', all(brainOne)));
const full = lumoState(all(brainOne, voices, eyes));
eq('everything that exists installed: 3 of 5', [full.ready, full.total], [3, 5]);
eq('… waving', full.pose, 'wave');
eq('… talking still needs the Ear and the agent', full.canTalk, false);
eq('the Ear and the Hands are coming soon', full.pieces.filter((p) => p.soon).map((p) => p.id), ['ear', 'hands']);
yes('the Ear is never ready today', !pieceReady('ear', all(brainOne, voices, eyes)));

// ---- search and filters ----
const list = all(brainOne, voices, eyes);
const capName = (c) => ({ brain: 'Cerebro', ear: 'Oído', voice: 'Voz', eye: 'Visión', img: 'Imagen' }[c]);
const label = (e) => e.name || (e.kind === 'voiceLang' ? `Español · ${e.voice}` : e.code);
eq('filters', AI_FILTERS, ['all', 'installed', 'brain', 'ear', 'voice', 'eye', 'img']);
const n = filterCounts(list, '', label, capName);
eq('counts: all', n.all, list.length);
eq('counts: installed', n.installed, list.filter((e) => e.state === 'installed').length);
eq('counts: per capability', [n.brain, n.ear, n.voice, n.eye, n.img], [3, 3, 7, 35, 2]);
eq('the counts add up', n.brain + n.ear + n.voice + n.eye + n.img, n.all);
eq('search: whisper', filterEntries(list, { query: 'whisper', label, capName }).map((e) => e.id), ['ear:whisper-base', 'ear:whisper-small', 'ear:whisper-turbo']);
eq('search: a capability\'s translated name, without accents', filterEntries(list, { query: 'oido', label, capName }).length, 3);
eq('search: every word', filterEntries(list, { query: 'qwen 4b', label, capName }).map((e) => e.id), ['llm:qwen3-4b']);
eq('search with a filter', filterEntries(list, { filter: 'installed', query: 'qwen', label, capName }).map((e) => e.id), ['llm:qwen3-1.7b']);
eq('search: by source', filterEntries(list, { query: 'xinntao', label, capName }).length, 2);
yes('search: nothing', !matchesQuery(list[0], 'zzz', label, capName));
eq('counts follow the search', filterCounts(list, 'whisper', label, capName), { all: 3, installed: 0, brain: 0, ear: 3, voice: 0, eye: 0, img: 0 });
eq('grouped in capability order', groupByCap(filterEntries(list, { query: 'real', label, capName })).map(([c]) => c), ['img']);
eq('groups: every capability', groupByCap(list).map(([c]) => c), CAPS);
eq('card: brain', capCard('brain', list), { cap: 'brain', count: 3, installed: 2, soon: false });
eq('card: ear is coming soon', capCard('ear', list).soon, true);
eq('card: img is coming soon', capCard('img', list).soon, true);
eq('sort: in use, recommended, installed, the rest', sortForPage([
  { id: 'a', state: 'cloud' }, { id: 'b', state: 'installed' }, { id: 'c', recommended: true, state: 'cloud' }, { id: 'd', inUse: true, state: 'installed' },
]).map((e) => e.id), ['d', 'c', 'b', 'a']);

// ---- space ----
eq('space: the folders measured', modelBytes({ voiceSys: 100, voiceUser: 20, llm: 3, ocr: 0 }), 123);
eq('space: unmeasured and bad values count 0', modelBytes({ a: NaN, b: -5, c: undefined, d: 7 }), 7);
eq('space: nothing measured yet', modelBytes(null), 0);

// a search finds the functions by their translated name and description (the maintainer searched «Escritura»)
const esName = { writing: 'Escritura', sentinela: 'Sentinela', notes: 'Notas' };
const esDesc = { writing: '«Corregí este texto»: corregir, mejorar o traducir', sentinela: 'Vigila los procesos' };
const fnIds = (q) => matchFunctions(FUNCTIONS, q, (f) => esName[f.id] || f.id, (f) => esDesc[f.id] || '').map((f) => f.id);
eq('«Escritura» finds Writing', fnIds('Escritura'), ['writing']);
eq('accents and case don\'t matter', fnIds('escritura'), ['writing']);
eq('by its description', fnIds('traducir'), ['writing']);
eq('Sentinela by name', fnIds('sentinela'), ['sentinela']);
eq('no query: none listed as results', fnIds('  '), []);
eq('no match', fnIds('whisper'), []);
// the brain's sheet: who uses the model, and when it can be tried (not Sentinela in particular)
eq('the functions that use the model', llmFunctions().map((f) => f.id), ['writing', 'sentinela']);
eq('tried with Writing on by default', llmTryReady({ installed: true, engine: true, sentinelaOn: false, writingChoice: null }), true);
eq('tried with Sentinela on and Writing off', llmTryReady({ installed: true, engine: true, sentinelaOn: true, writingChoice: false }), true);
eq('not with both off', llmTryReady({ installed: true, engine: true, sentinelaOn: false, writingChoice: false }), false);
eq('not without the model or the engine', [llmTryReady({ installed: false, engine: true, writingChoice: null }), llmTryReady({ installed: true, engine: false, writingChoice: null })], [false, false]);
// (0.8.9.6.2) where the Brain computes
eq('the choices', BRAIN_BACKENDS, ['auto', 'cpu', 'gpu']);
eq('auto by default', [brainBackend({}), brainBackend(null), brainBackend({ brain: { backend: 'tpu' } }), brainBackend({ brain: { backend: 'gpu' } })], ['auto', 'auto', 'auto', 'gpu']);
const vk = { backend: 'vulkan', device: 'AMD Radeon RX 580 Series', layers: 29, layersTotal: 29, gpus: { vulkan: true, cuda: false } };
eq('on the GPU, every layer', brainWhere(vk, 'auto'), { key: 'brainGpu', vars: { api: 'Vulkan', device: 'AMD Radeon RX 580 Series' } });
eq('on the GPU, some layers', brainWhere({ ...vk, layers: 20 }, 'gpu'), { key: 'brainGpuPart', vars: { api: 'Vulkan', device: 'AMD Radeon RX 580 Series', layers: 20, total: 29 } });
eq('CUDA without a name', brainWhere({ backend: 'cuda', device: null, layers: 0, layersTotal: 0 }, 'auto'), { key: 'brainGpu', vars: { api: 'CUDA', device: 'CUDA' } });
eq('nonsense layers ignored', brainWhere({ ...vk, layers: 99, layersTotal: 29 }, 'auto').key, 'brainGpuPart');
eq('processor chosen', brainWhere({ backend: 'cpu', fallback: null, gpus: { vulkan: true } }, 'cpu').key, 'brainCpuChosen');
eq('fallbacks say why', ['start', 'memory', 'untested', 'none', null].map((f) => brainWhere({ backend: 'cpu', fallback: f }, 'auto').key),
  ['brainCpuStart', 'brainCpuMemory', 'brainCpuUntested', 'brainCpuNoGpu', 'brainCpu']);
eq('not run yet: the GPU it will use', brainWhere({ backend: null, gpus: { vulkan: true, cuda: true } }, 'auto'), { key: 'brainGpuReady', vars: { api: 'CUDA' } });
eq('not run yet, no GPU', brainWhere({ backend: null, gpus: { vulkan: false, cuda: false } }, 'gpu').key, 'brainCpuNoGpu');
eq('no engine state at all', brainWhere(null, 'auto').key, 'brainCpuNoGpu');
console.log(`ai-store-test: ${ok} passed, ${bad} failed`);
process.exit(bad ? 1 : 0);
