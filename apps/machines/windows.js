// windows.js — Windows 11 in the New Machine assistant (docs/VIRTUALIZATION.md 4.3): pure functions over the catalog
// (assets/machines/systems.json, the "windows" entry) and the host's settings, tested by server/tests/vm_windows_test.mjs.
//
// Two ways in, both from Microsoft's own servers and nothing of Microsoft's kept by LordOS:
//   own   the full multi-edition ISO the person downloads from Microsoft's page; Machines checks its SHA-256 against
//         the list Microsoft publishes there (pinned in the catalog) and installs the edition their key is for
//   eval  Microsoft's 90-day Enterprise Evaluation, downloaded by the admin helper from Microsoft's server and kept
//         only with the hash Microsoft publishes for it
// The machine is a template that Windows 11 accepts as it is: UEFI with secure boot, a TPM 2.0, Hyper-V
// enlightenments, virtio disks and network with the virtio-win drivers Setup loads by itself.

export const WIN_MIN = { vcpus: 2, memoryMb: 4096, diskGb: 64 };

// the catalog's Windows entry for an architecture, or null
export function windowsEntry(cat, arch) {
  const s = (cat?.systems || []).find((x) => x?.id === 'windows');
  const a = s?.arch?.[arch];
  return a ? { ...a, editions: Array.isArray(s.editions) ? s.editions : [], name: s.name, release: s.release } : null;
}

// the virtio-win drivers' disc for an architecture: { name, sha, size } of the catalog's pinned file, or null
export function driversFor(cat, arch) {
  const s = (cat?.systems || []).find((x) => x?.id === 'virtio-win');
  const f = s?.arch?.[arch]?.pinned?.[0];
  return f ? { id: 'virtio-win', name: f.name, sha: f.sha256, size: f.bytes } : null;
}

// an ISO's SHA-256 against Microsoft's published list: { lang, label } of its language, or null
export function matchIso(entry, sha) {
  const h = String(sha || '').toLowerCase();
  const i = (entry?.isos || []).find((x) => x.sha256 === h);
  return i ? { lang: i.lang, label: i.label } : null;
}

// the evaluation's languages ({ lang, name, sha, size }) and the one for this person's language
export function evalFiles(entry) {
  return (entry?.pinned || []).map((f) => ({ lang: f.lang, name: f.name, sha: f.sha256, size: f.bytes }));
}
export function evalFor(entry, uiLang) {
  const files = evalFiles(entry);
  const l = String(uiLang || 'en').toLowerCase();
  const base = l.split(/[-_]/)[0];
  return files.find((f) => f.lang.toLowerCase() === l.replace('_', '-')) || files.find((f) => f.lang.toLowerCase().startsWith(`${base}-`)) ||
    files.find((f) => f.lang === 'en-US') || files[0] || null;
}

// the newest Windows ISO in a folder listing ([{ name, size, mtime }]), to offer the one just downloaded from
// Microsoft's page: its files are named Win11_<version>_<language>_x64[v<n>].iso (Arm64 for arm64)
export function newestDownload(entries, arch) {
  const re = arch === 'arm64' ? /^Win11_.*_Arm64.*\.iso$/i : /^Win11_.*_x64.*\.iso$/i;
  const list = (entries || []).filter((e) => e && !e.dir && re.test(e.name) && e.size > 1e9);
  list.sort((a, b) => (b.mtime || 0) - (a.mtime || 0));
  return list[0] || null;
}

// IANA time zone → Windows' own name (CLDR's table, assets/machines/windows-zones.json); "" when it isn't there.
// Names with three parts are also looked up without the middle one, as CLDR keeps them (America/Argentina/Buenos_Aires
// is America/Buenos_Aires there).
export function windowsZone(zones, tz) {
  const z = zones?.zones || zones || {};
  const t = String(tz || '');
  if (z[t]) return z[t];
  const parts = t.split('/');
  if (parts.length === 3 && z[`${parts[0]}/${parts[2]}`]) return z[`${parts[0]}/${parts[2]}`];
  if (t === 'UTC' || t === 'Etc/UTC' || t === 'Universal') return 'UTC';
  return '';
}

// xkb layout (and variant) → Windows' input locale "LLLL:KKKKKKKK"; the system language's own keyboard when unknown
const KEYBOARDS = {
  us: '0409:00000409', 'us:intl': '0409:00020409', gb: '0809:00000809', es: '040a:0000040a', latam: '080a:0000080a',
  br: '0416:00010416', pt: '0816:00000816', de: '0407:00000407', fr: '040c:0000040c', it: '0410:00000410', nl: '0413:00020409',
  ru: '0419:00000419', pl: '0415:00000415', tr: '041f:0000041f', se: '041d:0000041d', no: '0414:00000414', dk: '0406:00000406',
  fi: '040b:0000040b', cz: '0405:00000405', hu: '040e:0000040e',
};
export function inputLocale(keymap, variant, uiLang) {
  const k = String(keymap || '').toLowerCase(), v = String(variant || '').toLowerCase();
  return KEYBOARDS[`${k}:${v}`] || KEYBOARDS[k] || String(uiLang || 'en-US');
}

// this computer's formats for Windows ("es_AR.UTF-8" → "es-AR"); "" for C/POSIX
export function userLocale(locale) {
  const m = /^([a-z]{2,3})_([A-Z]{2})/.exec(String(locale || ''));
  return m ? `${m[1]}-${m[2]}` : '';
}

// the edition to install: { name, key } — the person's key when given, else Microsoft's KMS client key for that
// edition (it installs and stays unactivated); Home has none and needs the person's key. Evaluation media hold one
// image: no name, no key.
export function editionPlan(entry, mode, edition, key) {
  if (mode === 'eval') return { name: '', key: '', ok: true };
  const e = (entry?.editions || []).find((x) => x.name === edition) || entry?.editions?.[0];
  if (!e) return { name: '', key: '', ok: false };
  const k = normalizeKey(key);
  if (key && !k) return { name: e.name, key: '', ok: false, err: 'key' };
  if (k) return { name: e.name, key: k, ok: true };
  if (!e.kms) return { name: e.name, key: '', ok: false, err: 'needsKey' };
  return { name: e.name, key: e.kms, ok: true, generic: true };
}

// a product key as XXXXX-XXXXX-XXXXX-XXXXX-XXXXX (spaces and case forgiven), or ''
export function normalizeKey(s) {
  const raw = String(s || '').toUpperCase().replace(/[\s-]/g, '');
  if (!/^[A-Z0-9]{25}$/.test(raw)) return '';
  return raw.match(/.{5}/g).join('-');
}

// the account and computer for Windows' unattended install, as vmunattend.c checks them
export function checkWindowsSetup(u) {
  const errors = {};
  const user = String(u?.user || '');
  if (!/^[A-Za-z0-9._ -]{1,20}$/.test(user) || /^[ .]|[ .]$/.test(user)) errors.user = 'winUser';
  const host = String(u?.hostname || '');
  if (!/^[A-Za-z0-9][A-Za-z0-9-]{0,14}$/.test(host) || /^[0-9]+$/.test(host)) errors.hostname = 'winHost';
  if (String(u?.password || '').length > 127) errors.password = 'tooLong';
  return { ok: !Object.keys(errors).length, errors };
}

// a computer name from the machine's name: letters, digits and '-', at most 15
export function windowsHostname(name) {
  const s = String(name || '').normalize('NFKD').replace(/[̀-ͯ]/g, '').replace(/[^A-Za-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 15).replace(/-+$/, '');
  return s && !/^[0-9]+$/.test(s) ? s.toUpperCase() : 'WINDOWS';
}

// a profile's size, raised to what Windows 11 asks for
export function windowsSize(size, host) {
  const vcpus = Math.min(Math.max(size.vcpus, WIN_MIN.vcpus), Math.max(1, host?.cpus || WIN_MIN.vcpus));
  const memoryMb = Math.max(size.memoryMb, WIN_MIN.memoryMb);
  const diskGb = Math.max(size.diskGb, WIN_MIN.diskGb);
  return { ...size, vcpus, memoryMb, diskGb };
}
