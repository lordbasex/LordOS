// describe.js — what a program or an app is, in one place (docs/MONITOR.md, "Descriptions"): Monitor's Get Info shows it,
// and the Store, Settings → Apps and the Dock's and launcher's tooltips can ask the same function.
//
//   describeProgram(p, { lang })   p: a process of lord.monitor.processes ({ name, exe, app, prot, pkg?, unit? })
//   describeApp(item, { lang })    item: a launcher entry (useLauncherApps) or an app id
// Both return { text, long, lang, src, pkg, pkgText, english } | { pending: true } | { unknown: true }:
//   text     one line; long: a package's longer text (paragraphs), or '';
//   lang     the language the text is in ('en' when no translation exists for free);
//   src      where it comes from: 'kernel' | 'lordos' | 'program' (sdk/programdesc.json, English) | 'app' (a LordOS app's
//            manifest) | 'desktop' (a .desktop Comment, localized) | 'flatpak' (its exported entry's Comment, localized)
//            | 'web' | 'man' (its manual page's NAME, a translated page first) | 'unit' (a systemd unit's Description=)
//            | 'pkg' (the installed package's description, Debian's translation when apt has it) | 'lumo' (translated
//            by Sentinela's local model, kept in ~/.cache/lordos/descriptions/<lang>.json);
//   pkg, pkgText: the package that owns the program and its one line, when the main text is another one's;
//   english  the text is English while the interface isn't (offer "Translate with Lumo").
// pending: lord-server's worker thread is still reading (lord.monitor.describe; ask again a moment later). Nothing is
// run, nothing goes to the network: dpkg's database, apt's translation lists, manual pages, units and .desktop entries.
import { createDescriber } from './describemodel.js';
import { getApp } from './windows.js';
import { webAppEntry } from './webapps.js';
import * as fs from './fs.js';

export { translationKey, kernelText, curatedText } from './describemodel.js';

const CACHE_DIR = '.cache/lordos/descriptions';
let cacheDirMade = false;
const d = createDescriber({
  native: (kind, arg, lang) => {
    try {
      if (typeof lord === 'undefined' || typeof lord.monitor?.describe !== 'function' || !arg) return false;
      return lord.monitor.describe(kind, String(arg), lang);       // object, false (nothing), null (being read)
    } catch (e) { return false; }
  },
  getApp,
  webAppEntry,
  readCache: (lang) => fs.read(`${CACHE_DIR}/${lang}.json`),
  writeCache: (lang, text) => {
    if (!cacheDirMade) { for (const dir of ['.cache', '.cache/lordos', CACHE_DIR]) { try { fs.mkdir(dir); } catch (e) { /* there */ } } cacheDirMade = true; }
    // on a worker: the memory copy is the one read; a busy disk never holds the interface (QA ws174)
    fs.writeAsync(`${CACHE_DIR}/${lang}.json`, text).catch(() => { /* kept in memory */ });
  },
});
export const { describeProgram, describeApp, saveLumoTranslation } = d;
