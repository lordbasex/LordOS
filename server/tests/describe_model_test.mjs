// describe_model_test.mjs — sdk/describemodel.js (docs/MONITOR.md, "Descriptions") with made-up lookups: the order of
// the sources, kernel threads only for the kernel, well-known programs only in system folders, the language chosen
// (Debian's translation, a .desktop Comment[lang]) and the fallback to English marked as such, Lumo's kept translations,
// lookups still running, nothing known, and a LordOS app's manifest. Run: node tests/describe_model_test.mjs
import { createDescriber, kernelText, curatedText, translationKey, lordosHelpers } from '../../sdk/describemodel.js';

let n = 0, bad = 0;
const check = (name, ok, detail = '') => { n++; if (!ok) { bad++; console.log(`✗ ${name} ${detail}`); } };

// made-up lord-server answers: kind:arg:lang → object | false | null (still reading)
let answers = {};
const native = (kind, arg, lang) => (`${kind}:${arg}:${lang}` in answers ? answers[`${kind}:${arg}:${lang}`] : false);
const caches = {};
const apps = { monitor: { id: 'monitor', description: 'CPU, memory, network, disks, processes and each app\'s connections' } };
const d = createDescriber({
  native, getApp: (id) => apps[id] || null, webAppEntry: (id) => (id === 'cal' ? { id, name: 'Calendar web' } : null),
  readCache: (lang) => caches[lang] ?? '{}', writeCache: (lang, text) => { caches[lang] = text; },
});

// kernel threads
check('kworker', /worker thread/.test(kernelText('kworker/3:2-events')));
check('kswapd0 by its prefix', /memory reclaimer/.test(kernelText('kswapd0')));
check('card1-crtc0', /graphics driver/.test(kernelText('card1-crtc0')));
check('a longer word is not a prefix match', kernelText('cardigan') === null);
let r = d.describeProgram({ name: 'kworker/0:1', prot: 'kernel' }, { lang: 'es' });
check('a kernel thread: kernel, English, marked', r.src === 'kernel' && r.english === true && r.lang === 'en', JSON.stringify(r));
r = d.describeProgram({ name: 'kworker/0:1', exe: '/home/u/.cache/kworker', prot: null }, { lang: 'en' });
check('a program calling itself kworker never gets the kernel\'s text', r.unknown === true, JSON.stringify(r));

// well-known programs and LordOS's
check('pipewire in /usr/bin', curatedText({ exe: '/usr/bin/pipewire' })?.src === 'program');
check('pipewire in the home folder: not', curatedText({ exe: '/home/u/bin/pipewire' }) === null);
check('lord-media', curatedText({ exe: '/usr/lib/lordos/lord-media' })?.src === 'lordos');
check('a lord-* in the home folder: not', curatedText({ exe: '/home/u/lord-media' }) === null);
check('every LordOS program listed has a text', lordosHelpers().length > 30);

// a package's description: Debian's translation when lord-server found it, English marked otherwise
answers = { 'pkg:bash:es': { text: 'El intérprete de órdenes Bourne Again SHell de GNU', long: 'Bash es…', lang: 'es' } };
r = d.describeProgram({ name: 'helper', exe: '/usr/libexec/helper', pkg: 'bash' }, { lang: 'es' });
check('the translated package description', r.src === 'pkg' && r.lang === 'es' && r.english === false && /intérprete/.test(r.text), JSON.stringify(r));
answers = { 'pkg:bash:fr': { text: 'GNU Bourne Again SHell', long: '', lang: 'en' } };
r = d.describeProgram({ name: 'helper', exe: '/usr/libexec/helper', pkg: 'bash' }, { lang: 'fr' });
check('no French translation: English, marked to translate', r.src === 'pkg' && r.lang === 'en' && r.english === true, JSON.stringify(r));
r = d.describeProgram({ name: 'helper', exe: '/usr/libexec/helper', pkg: 'bash' }, { lang: 'en' });
check('in English for an English interface: nothing to translate', r.unknown === true || r.english === false);
answers['pkg:bash:en'] = { text: 'GNU Bourne Again SHell', long: '', lang: 'en' };
r = d.describeProgram({ name: 'helper', exe: '/usr/libexec/helper', pkg: 'bash' }, { lang: 'en' });
check('English interface', r.english === false && r.lang === 'en');

// Lumo's translation, kept per language and description
const key = translationKey({ src: 'pkg', of: 'bash' });
d.saveLumoTranslation(key, 'fr', 'Le shell GNU Bourne Again');
r = d.describeProgram({ name: 'helper', exe: '/usr/libexec/helper', pkg: 'bash' }, { lang: 'fr' });
check('Lumo\'s translation is used, the original kept', r.src === 'lumo' && r.text === 'Le shell GNU Bourne Again' && r.original === 'GNU Bourne Again SHell' && !r.english, JSON.stringify(r));
check('and kept in the language\'s cache', JSON.parse(caches.fr)[key] === 'Le shell GNU Bourne Again');

// the order: a manual page's line (the program's own) before its package's, which becomes the second line
answers = { 'man:sleep:es': { text: 'espera durante un tiempo especificado', lang: 'es' }, 'pkg:coreutils:es': { text: 'utilidades básicas de GNU', lang: 'es' } };
r = d.describeProgram({ name: 'sleep', exe: '/opt/x/sleep', pkg: 'coreutils' }, { lang: 'es' });
check('the manual page first, the package as a second line', r.src === 'man' && r.pkgText === 'utilidades básicas de GNU' && r.pkg === 'coreutils', JSON.stringify(r));
answers = { 'unit:demo.service:es': { text: 'A made-up daemon', lang: 'en' } };
r = d.describeProgram({ name: 'demod', exe: '/usr/sbin/demod', unit: 'demo.service' }, { lang: 'es' });
check('a service\'s Description=', r.src === 'unit' && r.english === true, JSON.stringify(r));

// still being read, and nothing known
answers = { 'man:tool:es': null };
r = d.describeProgram({ name: 'tool', exe: '/usr/bin/tool' }, { lang: 'es' });
check('still being read: pending', r.pending === true, JSON.stringify(r));
answers = {};
r = d.describeProgram({ name: 'miner', exe: '/home/u/.cache/miner' }, { lang: 'es' });
check('nothing known: unknown', r.unknown === true, JSON.stringify(r));

// apps: a .desktop Comment in the person's language or not, a LordOS app, a web app
const launcher = [{ id: 'desktop:gimp', desktopId: 'gimp', kind: 'linux', comment: 'Crear imágenes y editar fotografías', commentLocal: true },
  { id: 'desktop:foo', desktopId: 'foo', kind: 'linux', comment: 'A made-up app', commentLocal: false }];
r = d.describeProgram({ name: 'gimp', exe: '/usr/bin/gimp', app: 'desktop:gimp' }, { lang: 'es', apps: launcher });
check('a .desktop Comment[es]', r.src === 'desktop' && r.lang === 'es' && !r.english, JSON.stringify(r));
r = d.describeProgram({ name: 'foo', exe: '/usr/bin/foo', app: 'desktop:foo' }, { lang: 'es', apps: launcher });
check('a .desktop Comment only in English: marked', r.src === 'desktop' && r.english === true, JSON.stringify(r));
r = d.describeApp('monitor', { lang: 'pt' });
check('a LordOS app: its manifest, English marked', r.src === 'app' && /processes/.test(r.text) && r.english === true, JSON.stringify(r));
r = d.describeApp(launcher[0], { lang: 'es' });
check('describeApp for a Linux app', r.src === 'desktop' && !r.english);
r = d.describeProgram({ name: 'lord-web', exe: '/usr/lib/lordos/lord-web', app: 'desktop:lordos-web-cal' }, { lang: 'en' });
check('LordOS\'s own program wins over its scope', r.src === 'lordos', JSON.stringify(r));

console.log(`describe-model-test: ${n} checks, ${bad} failures`);
process.exit(bad ? 1 : 0);
