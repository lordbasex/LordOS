// localename.js — glibc region names, between what /usr/share/i18n/SUPPORTED lists and what LANG takes.
//
// SUPPORTED names most UTF-8 regions with the codeset ("en_GB.UTF-8") and some without it ("en_IN",
// "ca_ES@valencia"). A region is shown and stored without it ("en_IN", "ca_ES@valencia"); LANG, the
// admin actions locale-gen and system-locale, and the installer always get it, before the modifier as
// glibc writes it ("ca_ES.UTF-8@valencia"). The root side accepts both spellings (server/src/localename.h).
// Pure functions: tests in server/tests/localename_test.mjs.

// "en_GB.UTF-8" → "en_GB", "ca_ES.UTF-8@valencia" → "ca_ES@valencia", "en_IN" → "en_IN"
export const regionOf = (name) => String(name || '').replace(/\.utf-?8(?=@|$)/i, '');

// "en_GB" → "en_GB.UTF-8", "ca_ES@valencia" → "ca_ES.UTF-8@valencia"
export function localeOf(region) {
  const r = regionOf(region);
  const at = r.indexOf('@');
  return at < 0 ? `${r}.UTF-8` : `${r.slice(0, at)}.UTF-8${r.slice(at)}`;
}

// The country of a region ("en_AU" → "AU", "ca_ES@valencia" → "ES"), '' when it has none.
export const regionCountry = (region) => (/^[a-z]{2,3}_([A-Z]{2})(?=[.@]|$)/.exec(String(region || '')) || [])[1] || '';

// The interface language for a glibc locale, among the languages LordOS has (ids): its language part
// ("pt_BR.UTF-8" → "pt"), or for Chinese the script the country writes ("zh_TW", "zh_HK", "zh_MO" →
// "zh-Hant"; "zh_CN", "zh_SG" → "zh-Hans"), or the only id of that language with a script or variant
// ("xx" → "xx-Yyyy"). '' when LordOS has none: the caller uses English.
const TRADITIONAL = ['TW', 'HK', 'MO'];
export function languageOfLocale(name, ids) {
  const r = regionOf(name);
  const ll = (/^([a-z]{2,3})(?=[_.@]|$)/.exec(r) || [])[1];
  if (!ll || !Array.isArray(ids)) return '';
  if (ids.includes(ll)) return ll;
  if (ll === 'zh') {
    const want = TRADITIONAL.includes(regionCountry(r)) ? 'zh-Hant' : 'zh-Hans';
    if (ids.includes(want)) return want;
  }
  const variants = ids.filter((id) => typeof id === 'string' && id.startsWith(`${ll}-`));
  return variants.length ? variants[0] : '';
}

// The glibc language part of an interface language: "zh-Hant" → "zh", "pt" → "pt" (regions are
// named with it: zh_TW, pt_BR).
export const localeLanguage = (id) => String(id || '').split('-')[0];
