// err-check.mjs — checks that the server reports errors and statuses as codes, not prose.
//
//   node tools/err-check.mjs            (also run by tools/i18n-check.mjs)
//
// Scans server/src (C, without comments) and reports:
//   - codes used with admin_err/admin_ok/admin_log/admin_code, lerr_set, js_throw_code,
//     js_throw_line or put_line(…, "ERR", …) that have no English string in
//     assets/locales/en/errors.json (ERR) or status.json (OK/LOG);
//   - prose in ERR/OK/LOG lines (admin_say("ERR", "could not…")) and in JS exceptions
//     (JS_ThrowTypeError(c, "…")): they must be js_throw_code / admin_err with a code;
//   - Spanish prose in any string literal (the project is English-only; UI text goes in the
//     locale files).
// A line can opt out with a trailing /* i18n-ok */ comment (data, not prose: folder names,
// protocol tokens of other programs…). Files that other work still has to convert are listed in
// PENDING: their findings are warnings. See docs/LANGUAGES.md, "Error and status messages".
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const SRC = path.join(root, 'server/src');

// Out of this pass's scope (engine and app-mode work): warnings until converted.
export const PENDING = [
  'ui/paint.c', 'ui/layout.c', 'ui/text.c', 'ui/style.c',
];

const CODE = /^[a-z][a-zA-Z0-9]*(?:\.[a-zA-Z0-9]+)+$/;
const SPANISH = /[áéíóúñÁÉÍÓÚÑ¿¡]|\b(?:no se pudo|no hay|falta|faltan|inválid[oa]s?|desconocid[oa]s?|archivo|carpeta|contraseña|usuario|paquete|también|esencial|ayudante|conectad[oa]|desconectad[oa]|error al|demasiad[oa]s?|todavía|ningún|ninguna)\b/i;

// Replaces comments with spaces (keeping line numbers and the /* i18n-ok */ markers' lines).
function stripComments(src) {
  let out = '', i = 0;
  const ok = new Set();
  let line = 1;
  while (i < src.length) {
    const ch = src[i], nx = src[i + 1];
    if (ch === '"' || ch === "'") {
      const q = ch;
      out += ch; i++;
      while (i < src.length && src[i] !== q && src[i] !== '\n') {
        if (src[i] === '\\') { out += src[i] + (src[i + 1] ?? ''); i += 2; continue; }
        out += src[i++];
      }
      if (i < src.length) out += src[i++];
      continue;
    }
    if (ch === '/' && nx === '*') {
      const end = src.indexOf('*/', i + 2);
      const body = src.slice(i, end < 0 ? src.length : end + 2);
      if (/i18n-ok/.test(body)) ok.add(line);
      for (const c of body) { if (c === '\n') { out += '\n'; line++; } else out += ' '; }
      i += body.length;
      continue;
    }
    if (ch === '/' && nx === '/') {
      const end = src.indexOf('\n', i);
      const body = src.slice(i, end < 0 ? src.length : end);
      if (/i18n-ok/.test(body)) ok.add(line);
      out += ' '.repeat(body.length);
      i += body.length;
      continue;
    }
    if (ch === '\n') line++;
    out += ch; i++;
  }
  return { code: out, ok };
}

function walk(d, out = []) {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name);
    if (e.isDirectory()) walk(p, out);
    else if (/\.[ch]$/.test(e.name)) out.push(p);
  }
  return out;
}

const loadKeys = (ns) => {
  try { return JSON.parse(fs.readFileSync(path.join(root, 'assets/locales/en', `${ns}.json`), 'utf8')); } catch (e) { return {}; }
};

// The C string literal starting at s[i] ('"'), with adjacent literals joined: [text, end].
function literal(s, i) {
  let text = '';
  for (;;) {
    if (s[i] !== '"') break;
    i++;
    while (i < s.length && s[i] !== '"') { if (s[i] === '\\') { text += s[i] + s[i + 1]; i += 2; } else text += s[i++]; }
    i++;
    const m = /^\s*/.exec(s.slice(i, i + 200));
    if (s[i + m[0].length] === '"') i += m[0].length; else break;
  }
  return [text, i];
}

export function checkErrors({ quiet = false } = {}) {
  const errors = loadKeys('errors'), status = loadKeys('status');
  const problems = [], warnings = [];
  const used = new Map();       // code → { ns, where }
  for (const file of walk(SRC)) {
    const rel = path.relative(SRC, file);
    if (rel === 'errcode.h' || rel === 'ui/js_err.h') continue;          // documentation examples
    const pending = PENDING.some((p) => (p.endsWith('/') ? rel.startsWith(p) : rel === p));
    const { code: src, ok } = stripComments(fs.readFileSync(file, 'utf8'));
    const lineOf = (idx) => src.slice(0, idx).split('\n').length;
    const report = (idx, msg) => {
      const ln = lineOf(idx);
      if (ok.has(ln)) return;
      (pending ? warnings : problems).push(`server/src/${rel}:${ln}: ${msg}`);
    };
    const useCode = (idx, c, ns) => {
      if (!CODE.test(c)) { report(idx, `"${c}" is not a code (module.camelCase)`); return; }
      if (!used.has(c)) used.set(c, { ns, where: `server/src/${rel}:${lineOf(idx)}`, pending });
    };
    const scan = (re, fn) => { for (const m of src.matchAll(re)) fn(m); };

    // codes
    scan(/\badmin_(err|ok|log)\s*\(\s*(?=")/g, (m) => { const [c] = literal(src, m.index + m[0].length); useCode(m.index, c, m[1] === 'err' ? 'errors' : 'status'); });
    scan(/\badmin_code\s*\(\s*"(ERR|OK|LOG)"\s*,\s*(?=")/g, (m) => { const [c] = literal(src, m.index + m[0].length); useCode(m.index, c, m[1] === 'ERR' ? 'errors' : 'status'); });
    scan(/\blerr_set\s*\(\s*[^,;]+,\s*[^,;]+,\s*(?=")/g, (m) => { const [c] = literal(src, m.index + m[0].length); useCode(m.index, c, 'any'); });
    scan(/\bjs_throw_(?:code|line)\s*\(\s*\w+\s*,\s*(?=")/g, (m) => { const [c] = literal(src, m.index + m[0].length); useCode(m.index, c, 'errors'); });
    // put_line(id, "ERR", "…") (admin.c's own replies)
    scan(/\bput_line\s*\([^,;]+,\s*"ERR"\s*,\s*(?=")/g, (m) => {
      const [c] = literal(src, m.index + m[0].length);
      if (CODE.test(c)) useCode(m.index, c, 'errors'); else report(m.index, `prose in an ERR line: "${c}"`);
    });

    // prose in ERR/OK/LOG lines
    scan(/\badmin_say\s*\(\s*"(ERR|OK|LOG)"\s*,\s*(?=")/g, (m) => {
      const [t] = literal(src, m.index + m[0].length);
      if (t === '%s' && m[1] === 'LOG') return;                       // a program's output, as is
      report(m.index, `prose in an ${m[1]} line: "${t}" (use admin_${m[1] === 'ERR' ? 'err' : m[1].toLowerCase()} with a code)`);
    });
    // prose in JS exceptions
    scan(/\bJS_Throw(?:Type|Range|Internal|Syntax|Reference)?Error\s*\(\s*\w+\s*,\s*(?=")/g, (m) => {
      const [t] = literal(src, m.index + m[0].length);
      report(m.index, `JS exception with prose: "${t}" (use js_throw_code / js_throw_line)`);
    });
    // Spanish in any string literal (not in files whose strings are data in other languages: the localized
    // mailbox names a server may use for Sent, Trash, Junk… in mail/folderrole.c, #28; the common words say uses to
    // tell a text's language in tts/saytext.c)
    const LANG_DATA = new Set(['mail/folderrole.c', 'tts/saytext.c']);
    for (let i = LANG_DATA.has(rel) ? -1 : src.indexOf('"'); i >= 0; ) {
      if (src[i - 1] === "'" && src[i + 1] === "'") { i = src.indexOf('"', i + 1); continue; }   // '"'
      const [t, end] = literal(src, i);
      if (SPANISH.test(t)) report(i, `Spanish text in a string: "${t.length > 70 ? `${t.slice(0, 70)}…` : t}"`);
      i = src.indexOf('"', Math.max(end, i + 1));
    }
  }
  for (const [c, { ns, where, pending }] of used) {
    const inErr = c in errors, inStatus = c in status;
    const okKey = ns === 'errors' ? inErr : ns === 'status' ? inStatus : inErr || inStatus;
    if (!okKey) (pending ? warnings : problems).push(`${where}: code ${c} has no English string in ${ns === 'any' ? 'errors.json or status.json' : `${ns}.json`}`);
  }
  if (!quiet) {
    for (const w of warnings) console.log(`  (pending) ${w}`);
    for (const p of problems) console.log(`✗ ${p}`);
    console.log(`errors: ${used.size} codes in the server, ${problems.length} problems${warnings.length ? `, ${warnings.length} warnings in files still pending` : ''}`);
  }
  return { problems, warnings, used };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  process.exit(checkErrors().problems.length ? 1 : 0);
}
