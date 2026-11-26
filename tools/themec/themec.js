// themec.js — LordOS's theme compiler: checks a community theme's source and builds its code (docs/THEMES-GUIDE.md,
// "Community themes"; AGENTS.md "Community themes").
//
// lord-themec (server/src/themec/) runs this file in its own QuickJS, inside the helpers' sandbox, on the files of the
// .zip the person chose; server/tests/themec_test.mjs runs it in node. No dependencies and no module system: it
// defines globalThis.LordThemec.
//
//   LordThemec.compile({ id, files: [{ name, size, text? }] }) → {
//     report: { ok, id, errors, warnings, findings: [{ level, rule, file, line, message }], meta },
//     code,          the theme's code (a script for the theme's realm), when ok
//     keep,          the names of the files to install as they are (theme.json, assets, locales, licence texts)
//   }
//
// The checks are lordos-theme-kit's (scripts/check.mjs, MIT, the same project), run on the files in memory, plus what
// the compiler itself needs (no identifier starting with "__", no host elements in JSX). The build turns JSX into
// calls of the theme's React (createElement) and each file into a function of a tiny module table; imports resolve to
// the theme's own files or to what LordOS gives it (react, @lordos/sdk, @lordos/apps). The result runs in the theme's
// own realm (server/src/ui/js.c start_community), never in LordOS's.
(function (G) {
  'use strict';

  const VERSION = 1;
  const LIMITS = {
    files: 400, total: 20 * 1024 * 1024, code: 256 * 1024, json: 256 * 1024, themeJson: 4096, image: 5 * 1024 * 1024,
    text: 256 * 1024, line: 2000, stringLiteral: 4000, depth: 6, pathLength: 160, screenshots: 12, out: 4 * 1024 * 1024,
  };
  const LANGUAGES = ['ar', 'az', 'bg', 'bn', 'ca', 'de', 'el', 'en', 'es', 'fa', 'fi', 'fil', 'fr', 'hi', 'hu', 'id', 'it', 'ja', 'ko',
    'lt', 'ms', 'nl', 'pl', 'pt', 'ro', 'ru', 'sv', 'sw', 'th', 'tr', 'uk', 'ur', 'vi', 'zh-Hans', 'zh-Hant'];
  const CAPABILITIES = ['wifi', 'bluetooth', 'sound', 'battery', 'brightness', 'media', 'notifications'];
  const THEME_ID = /^[A-Za-z0-9_-]{1,26}$/;
  const SEGMENT = /^[A-Za-z0-9_][A-Za-z0-9._-]*$/;
  const CODE_EXT = ['.js', '.jsx'];
  const IMAGE_EXT = ['.png', '.jpg', '.jpeg'];
  const TEXT_NAMES = ['LICENSE', 'NOTICE', 'COPYING', 'AUTHORS'];
  const HOST_MODULES = ['react', '@lordos/sdk', '@lordos/apps'];
  // eslint-disable-next-line no-control-regex
  const UNSAFE_TEXT = /[\u0000-\u001f\u007f-\u009f‎‏‪-‮⁦-⁩]/;
  const LICENSE = /^[A-Za-z0-9.+-]{1,40}( (OR|AND|WITH) [A-Za-z0-9.+-]{1,40}){0,3}$/;
  const VERSION_RE = /^[0-9]{1,5}(\.[0-9]{1,5}){0,3}([-+][0-9A-Za-z.-]{1,20})?$/;
  const LORD_UI = new Set(['getLayout', 'setStyle', 'measureText', 'now', 'log', 'version']);
  const LORD_FORBIDDEN = {
    term: 'the terminal', fs: 'files', admin: 'administrator actions', perms: 'permission answers', pkg: 'packages',
    broker: "isolated apps' broker", apps: 'launching programs (use launchApp from @lordos/sdk)', vault: 'the password vault',
    greeter: 'the login screen', themes: 'switching themes', exec: 'running commands', spawn: 'running commands',
    codesig: 'the verified-code check', session: 'the session (use session from @lordos/sdk)', vm: 'virtual machines',
    clipboard: 'the clipboard', keyboard: 'keyboard layouts', net: 'the network (use the SDK models)', portal: 'the desktop portal',
    remote: 'remote access', rdesk: 'remote desktop', cloud: 'cloud accounts', mail: 'mail', settings: 'the settings file',
    wayland: "Linux apps' windows", sec: 'security', ssh: 'SSH', users: 'accounts', share: 'screen sharing', media: 'cameras and recordings',
  };
  const VISIBLE_PROPS = /^(label|accessibilityLabel|title|placeholder|hint|alt|tooltip|text|message|caption|description|subtitle|aria-label|confirmLabel|cancelLabel|okLabel|emptyText|header)$/;
  const WORDS = /\p{L}{2,}/u;
  const SHORTCUT = /^[❖⌥⌃⇧⌘↑↓←→\s]*((Ctrl|Alt|Shift|Meta|Super|Esc|Tab|Enter|Space|Del|Home|End|F\d{1,2}|[A-Z0-9])[+\s]?)+$/;

  const extOf = (n) => { const b = n.slice(n.lastIndexOf('/') + 1); const d = b.lastIndexOf('.'); return d > 0 ? b.slice(d).toLowerCase() : ''; };
  const baseOf = (n) => n.slice(n.lastIndexOf('/') + 1);
  const dirOf = (n) => (n.includes('/') ? n.slice(0, n.lastIndexOf('/')) : '');

  // ------------------------------------------------------------------------------------------------------------------
  // The scanner (lordos-theme-kit scripts/lib/lex.mjs): comments, strings, templates, regexes and JSX text found and
  // blanked out in `masked`, so the checks look at code only.
  const ID_START = /[A-Za-z_$\u0080-￿]/;
  const ID_PART = /[A-Za-z0-9_$\u0080-￿]/;
  const EXPR_WORDS = new Set(['return', 'typeof', 'instanceof', 'in', 'of', 'new', 'delete', 'void', 'throw', 'case', 'do', 'else', 'yield', 'await', 'default', 'export']);

  function unescapeJs(s) {
    return s.replace(/\\(u\{[0-9a-fA-F]+\}|u[0-9a-fA-F]{4}|x[0-9a-fA-F]{2}|[\s\S])/g, (m, e) => {
      if (e[0] === 'u' && e[1] === '{') { const c = parseInt(e.slice(2, -1), 16); return c <= 0x10ffff ? String.fromCodePoint(c) : ''; }
      if (e[0] === 'u' && e.length === 5) return String.fromCharCode(parseInt(e.slice(1), 16));
      if (e[0] === 'x' && e.length === 3) return String.fromCharCode(parseInt(e.slice(1), 16));
      const map = { n: '\n', t: '\t', r: '\r', b: '\b', f: '\f', v: '\v', 0: '\0', '\n': '' };
      return e in map ? map[e] : e;
    });
  }

  function scan(src) {
    const n = src.length;
    const out = src.split('');
    const strings = [], jsxText = [], errors = [];
    let i = 0, line = 1, prev = { t: 'start' };
    const blank = (a, b) => { for (let k = a; k < b; k++) if (out[k] !== '\n') out[k] = ' '; };
    const valueEnds = () => prev.t === 'id' || prev.t === ')' || prev.t === ']' || prev.t === 'str';
    const advance = (to) => { for (; i < to; i++) if (src[i] === '\n') line++; };
    function skip() {
      for (;;) {
        if (i >= n) return;
        const c = src[i];
        if (c === '\n') { line++; i++; continue; }
        if (c === ' ' || c === '\t' || c === '\r' || c === '\f' || c === '\v' || c === '﻿') { i++; continue; }
        if (c === '/' && src[i + 1] === '/') { const e = src.indexOf('\n', i); const end = e < 0 ? n : e; blank(i, end); i = end; continue; }
        if (c === '/' && src[i + 1] === '*') {
          const e = src.indexOf('*/', i + 2); const end = e < 0 ? n : e + 2;
          if (e < 0) errors.push({ line, message: 'unterminated comment' });
          blank(i, end); advance(end); continue;
        }
        return;
      }
    }
    function readString(q, kind, attr) {
      const start = i, l = line;
      i++;
      let v = '';
      while (i < n && src[i] !== q) {
        if (src[i] === '\\' && kind !== 'jsxattr') { v += src[i] + (src[i + 1] || ''); i += 2; continue; }
        if (src[i] === '\n') { if (kind !== 'jsxattr') { errors.push({ line, message: 'unterminated string' }); break; } line++; }
        v += src[i]; i++;
      }
      blank(start + 1, i);
      i++;
      strings.push({ value: kind === 'jsxattr' ? v : unescapeJs(v), raw: v, line: l, pos: start, kind, attr });
      prev = { t: 'str' };
    }
    function readTemplate() {
      i++;
      let segStart = i, v = '', whole = '', dynamic = false;
      const pos = i - 1, l0 = line;
      while (i < n) {
        const c = src[i];
        if (c === '\\') { v += c + (src[i + 1] || ''); i += 2; continue; }
        if (c === '`') { blank(segStart, i); whole += v; strings.push({ value: unescapeJs(whole), raw: whole, line: l0, pos, kind: 'template', dynamic }); i++; prev = { t: 'str' }; return; }
        if (c === '$' && src[i + 1] === '{') { blank(segStart, i); whole += v + '${…}'; dynamic = true; i += 2; prev = { t: 'start' }; code('}'); i++; segStart = i; v = ''; continue; }
        if (c === '\n') line++;
        v += c; i++;
      }
      errors.push({ line, message: 'unterminated template literal' });
    }
    function readRegex() {
      const start = i;
      i++;
      let cls = false;
      while (i < n) {
        const c = src[i];
        if (c === '\\') { i += 2; continue; }
        if (c === '\n') { errors.push({ line, message: 'unterminated regular expression' }); break; }
        if (c === '[') cls = true; else if (c === ']') cls = false; else if (c === '/' && !cls) break;
        i++;
      }
      blank(start + 1, i);
      i++;
      while (i < n && ID_PART.test(src[i])) i++;
      prev = { t: 'id' };
    }
    function readIdent() {
      const s = i;
      while (i < n && ID_PART.test(src[i])) i++;
      const w = src.slice(s, i);
      prev = EXPR_WORDS.has(w) ? { t: 'op' } : { t: 'id', w };
    }
    function jsxElement() {
      i++;
      skip();
      if (src[i] === '>') { i++; jsxChildren(); return; }
      while (i < n && /[A-Za-z0-9_$.:-]/.test(src[i])) i++;
      for (;;) {
        skip();
        if (i >= n) { errors.push({ line, message: 'unterminated JSX tag' }); return; }
        const c = src[i];
        if (c === '/' && src[i + 1] === '>') { i += 2; return; }
        if (c === '>') { i++; jsxChildren(); return; }
        if (c === '{') { i++; prev = { t: 'start' }; code('}'); i++; continue; }
        const s = i;
        while (i < n && /[A-Za-z0-9_$:-]/.test(src[i])) i++;
        const name = src.slice(s, i);
        if (!name) { i++; continue; }
        skip();
        if (src[i] === '=') {
          i++; skip();
          if (src[i] === '"' || src[i] === "'") readString(src[i], 'jsxattr', name);
          else if (src[i] === '{') { i++; prev = { t: 'start' }; const before = strings.length; code('}'); for (let k = before; k < strings.length; k++) if (!strings[k].attr) strings[k].attr = name; i++; }
          else if (src[i] === '<') jsxElement();
        }
      }
    }
    function jsxChildren() {
      let s = i, l = line;
      const flush = () => {
        const v = src.slice(s, i);
        if (v.trim()) jsxText.push({ value: v.replace(/\s+/g, ' ').trim(), pos: s, line: l + (v.match(/^\s*/)[0].split('\n').length - 1) });
        blank(s, i);
      };
      while (i < n) {
        const c = src[i];
        if (c === '<') {
          flush();
          if (src[i + 1] === '/') {
            const e = src.indexOf('>', i);
            if (e < 0) { errors.push({ line, message: 'unterminated JSX closing tag' }); i = n; return; }
            advance(e + 1); prev = { t: ')' }; return;
          }
          jsxElement(); s = i; l = line; continue;
        }
        if (c === '{') { flush(); i++; prev = { t: 'start' }; code('}'); i++; s = i; l = line; continue; }
        if (c === '\n') line++;
        i++;
      }
      errors.push({ line, message: 'unterminated JSX element' });
    }
    function code(until) {
      let depth = 0;
      while (i < n) {
        skip();
        if (i >= n) return;
        const c = src[i];
        if (until && depth === 0 && c === until) return;
        if (c === '"' || c === "'") { readString(c, 'string'); continue; }
        if (c === '`') { readTemplate(); continue; }
        if (ID_START.test(c)) { readIdent(); continue; }
        if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(src[i + 1] || ''))) { while (i < n && /[0-9A-Za-z_.]/.test(src[i])) i++; prev = { t: 'id' }; continue; }
        if (c === '/') { if (valueEnds()) { i++; prev = { t: 'op' }; } else readRegex(); continue; }
        if (c === '<' && !valueEnds() && /[A-Za-z>_$]/.test(src[i + 1] || '')) { jsxElement(); prev = { t: ')' }; continue; }
        if (c === '(' || c === '[' || c === '{') { depth++; i++; prev = { t: 'op' }; continue; }
        if (c === ')' || c === ']' || c === '}') { depth--; i++; prev = { t: c === '}' ? 'op' : c }; if (depth < 0) depth = 0; continue; }
        if (c === '=' && src[i + 1] === '>') { i += 2; prev = { t: 'op' }; continue; }
        if ((c === '+' || c === '-') && src[i + 1] === c) { i += 2; continue; }
        i++; prev = { t: 'op' };
      }
    }
    code(null);
    return { masked: out.join(''), strings, jsxText, errors };
  }

  // ------------------------------------------------------------------------------------------------------------------
  // The build: JSX → createElement calls, import/export → the module table.
  class BuildError extends Error {
    constructor(line, message) { super(message); this.line = line; }
  }

  const ENTITIES = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: ' ', hellip: '…', mdash: '—', ndash: '–', copy: '©', middot: '·', times: '×' };
  function decodeEntities(s) {
    return s.replace(/&(#x[0-9a-fA-F]{1,6}|#[0-9]{1,7}|[a-z]{2,8});/g, (m, e) => {
      if (e[0] === '#') { const c = e[1] === 'x' ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10); return c > 0 && c <= 0x10ffff ? String.fromCodePoint(c) : m; }
      return e in ENTITIES ? ENTITIES[e] : m;
    });
  }
  // JSX text as React reads it: lines trimmed, blank lines dropped, joined with a space
  function jsxTextValue(raw) {
    const lines = raw.split(/\r\n|\n|\r/);
    let last = -1;
    for (let k = 0; k < lines.length; k++) if (/[^ \t]/.test(lines[k])) last = k;
    let out = '';
    for (let k = 0; k < lines.length; k++) {
      let l = lines[k].replace(/\t/g, ' ');
      if (k > 0) l = l.replace(/^ +/, '');
      if (k < lines.length - 1) l = l.replace(/ +$/, '');
      if (!l) continue;
      if (k !== last && out) out += ' ';
      else if (out && k === last) out += ' ';
      out += l;
    }
    return decodeEntities(out);
  }

  function transform(src, file, resolve) {
    const n = src.length;
    let i = 0, line = 1, prev = { t: 'start' };
    const deps = [];               // specifiers resolved, in order
    const head = [];               // export getters, at the module's top
    const hoisted = [];            // const bindings of imports
    let mods = 0;
    const valueEnds = () => prev.t === 'id' || prev.t === ')' || prev.t === ']' || prev.t === 'str';
    const fail = (msg) => { throw new BuildError(line, msg); };

    function ws() {
      const s = i;
      for (;;) {
        if (i >= n) break;
        const c = src[i];
        if (c === '\n') { line++; i++; continue; }
        if (c === ' ' || c === '\t' || c === '\r' || c === '\f' || c === '\v' || c === '﻿') { i++; continue; }
        if (c === '/' && src[i + 1] === '/') { const e = src.indexOf('\n', i); i = e < 0 ? n : e; continue; }
        if (c === '/' && src[i + 1] === '*') { const e = src.indexOf('*/', i + 2); if (e < 0) fail('unterminated comment'); for (; i < e + 2; i++) if (src[i] === '\n') line++; continue; }
        break;
      }
      return src.slice(s, i);
    }
    function str(q) {
      const s = i;
      i++;
      while (i < n && src[i] !== q) {
        if (src[i] === '\\') { i += 2; continue; }
        if (src[i] === '\n') fail('unterminated string');
        i++;
      }
      if (i >= n) fail('unterminated string');
      i++;
      prev = { t: 'str' };
      return src.slice(s, i);
    }
    function template() {
      let o = '`';
      i++;
      while (i < n) {
        const c = src[i];
        if (c === '\\') { o += c + (src[i + 1] || ''); i += 2; continue; }
        if (c === '`') { i++; prev = { t: 'str' }; return `${o}\``; }
        if (c === '$' && src[i + 1] === '{') { i += 2; prev = { t: 'start' }; o += `\${${code('}')}}`; i++; continue; }
        if (c === '\n') line++;
        o += c; i++;
      }
      return fail('unterminated template literal');
    }
    function regex() {
      const s = i;
      i++;
      let cls = false;
      while (i < n) {
        const c = src[i];
        if (c === '\\') { i += 2; continue; }
        if (c === '\n') fail('unterminated regular expression');
        if (c === '[') cls = true; else if (c === ']') cls = false; else if (c === '/' && !cls) break;
        i++;
      }
      i++;
      while (i < n && ID_PART.test(src[i])) i++;
      prev = { t: 'id' };
      return src.slice(s, i);
    }
    function ident() { const s = i; while (i < n && ID_PART.test(src[i])) i++; return src.slice(s, i); }

    // a small token reader for import and export statements
    function tok() {
      ws();
      if (i >= n) return { t: 'eof' };
      const c = src[i];
      if (c === '"' || c === "'") { const raw = str(c); return { t: 'str', v: unescapeJs(raw.slice(1, -1)) }; }
      if (ID_START.test(c)) return { t: 'id', v: ident() };
      i++;
      return { t: 'p', v: c };
    }
    const expect = (t, v) => { const k = tok(); if (k.t !== t || (v !== undefined && k.v !== v)) fail(`expected ${v || t} in an import or export`); return k; };
    const plainName = (s) => /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(s);
    function modVar(spec) {
      const target = resolve(spec, file, line);
      deps.push(target);
      const v = `__m${mods++}`;
      hoisted.push(`const ${v} = __require(${JSON.stringify(target)});`);
      return v;
    }
    function getter(name, expr) { head.push(`Object.defineProperty(__exports, ${JSON.stringify(name)}, { enumerable: true, get: () => ${expr} });`); }

    function importStatement() {
      let k = tok();
      if (k.t === 'str') { modVar(k.v); semi(); return ''; }
      const binds = [];
      let ns = null;
      if (k.t === 'id' && k.v !== 'from') { binds.push(['default', k.v]); k = tok(); if (k.t === 'p' && k.v === ',') k = tok(); }
      if (k.t === 'p' && k.v === '*') { expect('id', 'as'); ns = expect('id').v; k = tok(); }
      else if (k.t === 'p' && k.v === '{') {
        for (;;) {
          k = tok();
          if (k.t === 'p' && k.v === '}') break;
          if (k.t !== 'id' && k.t !== 'str') fail('a name in braces');
          let local = k.v;
          const imported = k.v;
          k = tok();
          if (k.t === 'id' && k.v === 'as') { local = expect('id').v; k = tok(); }
          binds.push([imported, local]);
          if (k.t === 'p' && k.v === '}') break;
          if (!(k.t === 'p' && k.v === ',')) fail('"," or "}" in an import');
        }
        k = tok();
      }
      if (!(k.t === 'id' && k.v === 'from')) fail('"from" in an import');
      const spec = expect('str').v;
      semi();
      const v = modVar(spec);
      if (ns) { if (!plainName(ns)) fail('a plain name'); hoisted.push(`const ${ns} = ${v};`); }
      for (const [imp, local] of binds) {
        if (!plainName(local)) fail('a plain name');
        hoisted.push(`const ${local} = ${v}[${JSON.stringify(imp)}];`);
      }
      return '';
    }
    function semi() { const s = i, l = line; ws(); if (src[i] === ';') i++; else { i = s; line = l; } }

    function exportStatement() {
      const s0 = i, l0 = line;
      const k = tok();
      if (k.t === 'id' && k.v === 'default') {
        const s1 = i, l1 = line;
        const k2 = tok();
        if (k2.t === 'id' && (k2.v === 'function' || k2.v === 'class' || k2.v === 'async')) {
          let k3 = k2;
          if (k2.v === 'async') k3 = tok();
          ws();
          if (src[i] === '*') { i++; ws(); }
          const name = ID_START.test(src[i] || '') ? ident() : '';
          i = s1; line = l1;
          if (name && plainName(name)) { getter('default', name); prev = { t: 'start' }; return ''; }
          void k3;
        }
        i = s1; line = l1;
        prev = { t: 'op' };
        return '__exports.default = ';
      }
      if (k.t === 'id' && (k.v === 'function' || k.v === 'async' || k.v === 'class')) {
        let k2 = k;
        if (k.v === 'async') k2 = tok();
        if (k2.t !== 'id' || (k2.v !== 'function' && k2.v !== 'class')) fail('export async function');
        ws();
        if (src[i] === '*') { i++; ws(); }
        const name = ident();
        if (!plainName(name)) fail('an exported function needs a name');
        getter(name, name);
        i = s0; line = l0;
        prev = { t: 'start' };
        return '';
      }
      if (k.t === 'id' && (k.v === 'const' || k.v === 'let' || k.v === 'var')) {
        // export const a = …, b = …: the names (plain ones only) — found by scanning the declaration's top level
        const save = i, sl = line;
        const names = declaredNames();
        i = save; line = sl;
        for (const nm of names) getter(nm, nm);
        i = s0; line = l0;
        prev = { t: 'start' };
        return '';
      }
      if (k.t === 'p' && k.v === '{') {
        const list = [];
        for (;;) {
          let t = tok();
          if (t.t === 'p' && t.v === '}') break;
          if (t.t !== 'id') fail('a name in braces');
          const local = t.v;
          let exported = local;
          t = tok();
          if (t.t === 'id' && t.v === 'as') { const e = tok(); if (e.t !== 'id' && e.t !== 'str') fail('a name after "as"'); exported = e.v; t = tok(); }
          list.push([local, exported]);
          if (t.t === 'p' && t.v === '}') break;
          if (!(t.t === 'p' && t.v === ',')) fail('"," or "}" in an export');
        }
        const s2 = i, l2 = line;
        const t = tok();
        if (t.t === 'id' && t.v === 'from') {
          const v = modVar(expect('str').v);
          for (const [local, exported] of list) getter(exported, `${v}[${JSON.stringify(local)}]`);
        } else {
          i = s2; line = l2;
          for (const [local, exported] of list) { if (!plainName(local)) fail('a plain name'); getter(exported, local); }
        }
        semi();
        prev = { t: 'start' };
        return '';
      }
      return fail('export * is not supported: name what you export');
    }
    // the names a const/let/var declaration binds at its top level (plain names; a pattern is refused)
    function declaredNames() {
      const names = [];
      for (;;) {
        ws();
        if (!ID_START.test(src[i] || '')) fail('export a plain name (no destructuring)');
        names.push(ident());
        ws();
        if (src[i] !== '=') { if (src[i] === ',') { i++; continue; } break; }
        i++;
        // skip the initializer to the next top-level "," or the end of the statement
        let depth = 0;
        for (;;) {
          ws();
          if (i >= n) return names;
          const c = src[i];
          if (c === '"' || c === "'") { str(c); continue; }
          if (c === '`') { template(); continue; }
          if (c === '(' || c === '[' || c === '{') { depth++; i++; prev = { t: 'op' }; continue; }
          if (c === ')' || c === ']' || c === '}') { if (depth === 0) return names; depth--; i++; prev = { t: c === '}' ? 'op' : c }; continue; }
          if (depth === 0 && c === ',') { i++; break; }
          if (depth === 0 && c === ';') return names;
          if (ID_START.test(c)) { const w = ident(); prev = EXPR_WORDS.has(w) ? { t: 'op' } : { t: 'id' }; if (depth === 0 && (w === 'const' || w === 'let' || w === 'var' || w === 'function' || w === 'export' || w === 'import')) return names; continue; }
          if (c === '/') { if (valueEnds()) { i++; prev = { t: 'op' }; } else regex(); continue; }
          if (c === '<' && !valueEnds() && /[A-Za-z>_$]/.test(src[i + 1] || '')) { jsx(); prev = { t: ')' }; continue; }
          if (c === '\n') line++;
          i++;
          if (!/\s/.test(c)) prev = { t: 'op' };
        }
      }
      return names;
    }

    // ---- JSX ----
    function tagName() {
      let s = '';
      while (i < n && /[A-Za-z0-9_$.:-]/.test(src[i])) s += src[i++];
      return s;
    }
    function typeExpr(name) {
      if (name.includes(':') || name.includes('-')) fail(`<${name}>: no namespaced or hyphenated tags`);
      const first = name.split('.')[0];
      if (!name.includes('.') && /^[a-z]/.test(first)) fail(`<${name}>: a community theme draws with View, Text and Image from @lordos/sdk, never host elements`);
      if (!name.split('.').every(plainName)) fail(`<${name}>: not a component name`);
      return name;
    }
    function jsxAttrValue() {
      ws();
      const c = src[i];
      if (c === '"' || c === "'") {
        const s = i;
        i++;
        while (i < n && src[i] !== c) { if (src[i] === '\n') line++; i++; }
        if (i >= n) fail('unterminated attribute');
        i++;
        return JSON.stringify(decodeEntities(src.slice(s + 1, i - 1)));
      }
      if (c === '{') { i++; prev = { t: 'start' }; const e = code('}'); i++; if (!e.trim()) fail('an empty {} attribute'); return `(${e})`; }
      if (c === '<') return jsx();
      return fail('an attribute value');
    }
    function jsx() {
      const l0 = line;
      i++;
      ws();
      let type = 'React.Fragment';
      let name = '';
      const props = [];
      let selfClosing = false;
      if (src[i] === '>') { i++; type = '__Fragment'; }
      else {
        name = tagName();
        type = typeExpr(name);
        for (;;) {
          ws();
          if (i >= n) fail('unterminated JSX tag');
          const c = src[i];
          if (c === '/' && src[i + 1] === '>') { i += 2; selfClosing = true; break; }
          if (c === '>') { i++; break; }
          if (c === '{') {
            i++; ws();
            if (src.slice(i, i + 3) !== '...') fail('{…} in a tag must be a spread {...props}');
            i += 3; prev = { t: 'start' };
            props.push(`...(${code('}')})`);
            i++;
            continue;
          }
          const s = i;
          while (i < n && /[A-Za-z0-9_$:-]/.test(src[i])) i++;
          const an = src.slice(s, i);
          if (!an) fail(`unexpected "${c}" in a tag`);
          if (an.includes(':')) fail(`${an}: no namespaced attributes`);
          ws();
          const key = plainName(an) ? an : JSON.stringify(an);
          if (src[i] === '=') { i++; props.push(`${key}: ${jsxAttrValue()}`); } else props.push(`${key}: true`);
        }
      }
      const kids = [];
      if (!selfClosing) {
        let s = i;
        for (;;) {
          if (i >= n) { line = l0; fail(`unterminated <${name || ''}>`); }
          const c = src[i];
          if (c === '<') {
            const text = jsxTextValue(src.slice(s, i));
            if (text) kids.push(JSON.stringify(text));
            if (src[i + 1] === '/') {
              i += 2; ws();
              const close = tagName();
              ws();
              if (src[i] !== '>' || close !== name) fail(`</${close}> doesn't close <${name}>`);
              i++;
              break;
            }
            kids.push(jsx());
            s = i;
            continue;
          }
          if (c === '{') {
            const text = jsxTextValue(src.slice(s, i));
            if (text) kids.push(JSON.stringify(text));
            i++; prev = { t: 'start' };
            const e = code('}');
            i++;
            if (e.trim().startsWith('...')) fail('{...children} is not supported');
            if (e.replace(/\/\*[\s\S]*?\*\//g, '').trim()) kids.push(`(${e})`);
            s = i;
            continue;
          }
          if (c === '\n') line++;
          i++;
        }
      }
      prev = { t: ')' };
      return `__h(${type}, ${props.length ? `{ ${props.join(', ')} }` : 'null'}${kids.length ? `, ${kids.join(', ')}` : ''})`;
    }

    // ---- code: copied as it is, with JSX built and top-level imports and exports turned into the module table ----
    function code(until, top) {
      let o = '';
      let depth = 0;
      while (i < n) {
        o += ws();
        if (i >= n) break;
        const c = src[i];
        if (until && depth === 0 && c === until) return o;
        if (c === '"' || c === "'") { o += str(c); continue; }
        if (c === '`') { o += template(); continue; }
        if (ID_START.test(c)) {
          const memberOf = prev.t === '.';
          const w = ident();
          if (!memberOf && (w === 'import' || w === 'export')) {
            const save = i, sl = line;
            ws();
            const nx = src[i];
            i = save; line = sl;
            if (w === 'import' && (nx === '(' || nx === '.')) fail('dynamic import() and import.meta are not allowed');
            if (!top || depth !== 0) fail(`${w} only at a file's top level`);
            o += w === 'import' ? importStatement() : exportStatement();
            continue;
          }
          o += w;
          prev = EXPR_WORDS.has(w) ? { t: 'op' } : { t: 'id', w };
          continue;
        }
        if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(src[i + 1] || ''))) { const s = i; while (i < n && /[0-9A-Za-z_.]/.test(src[i])) i++; o += src.slice(s, i); prev = { t: 'id' }; continue; }
        if (c === '/') { if (valueEnds()) { i++; o += '/'; prev = { t: 'op' }; } else o += regex(); continue; }
        if (c === '<' && !valueEnds() && /[A-Za-z>_$]/.test(src[i + 1] || '')) { o += jsx(); continue; }
        if (c === '(' || c === '[' || c === '{') { depth++; i++; o += c; prev = { t: 'op' }; continue; }
        if (c === ')' || c === ']' || c === '}') { depth--; i++; o += c; prev = { t: c === '}' ? 'op' : c }; if (depth < 0) fail(`unbalanced "${c}"`); continue; }
        if (c === '.' && src[i + 1] !== '.') { i++; o += c; prev = { t: '.' }; continue; }
        if (c === '=' && src[i + 1] === '>') { i += 2; o += '=>'; prev = { t: 'op' }; continue; }
        if ((c === '+' || c === '-') && src[i + 1] === c) { i += 2; o += c + c; continue; }
        if (c === '?' && src[i + 1] === '.' && !/[0-9]/.test(src[i + 2] || '')) { i += 2; o += '?.'; prev = { t: '.' }; continue; }
        i++; o += c; prev = { t: 'op' };
      }
      if (until) fail(`missing "${until}"`);
      if (depth !== 0) fail('unbalanced brackets at the end of the file');
      return o;
    }

    const body = code(null, true);
    return { code: `${head.join('\n')}\n${hoisted.join('\n')}\n${body}`, deps };
  }

  // ------------------------------------------------------------------------------------------------------------------
  // The checks (lordos-theme-kit scripts/check.mjs, on files in memory)

  function checkFiles(files, add) {
    let total = 0, count = 0;
    const names = new Set();
    for (const f of files) {
      const segs = f.name.split('/');
      if (names.has(f.name)) { add('error', 'files.duplicate', f.name, null, 'the archive holds this file twice'); continue; }
      names.add(f.name);
      if (segs.some((s) => s === '..' || s === '.' || s === '') || f.name.startsWith('/') || f.name.includes('\\')) { add('error', 'files.name', f.name, null, 'no "..", "." or absolute names'); continue; }
      if (segs.some((s) => s.startsWith('.'))) { add('warning', 'files.hidden', f.name, null, 'hidden files are not installed'); continue; }
      if (segs.some((s) => !SEGMENT.test(s)) || f.name.length > LIMITS.pathLength) { add('error', 'files.name', f.name, null, 'use only letters, digits, ".", "-" and "_" in names'); continue; }
      if (segs.length > LIMITS.depth) add('error', 'files.depth', f.name, null, `at most ${LIMITS.depth} folder levels`);
      count++; total += f.size;
      const ext = extOf(f.name), base = baseOf(f.name);
      let max = 0;
      if (CODE_EXT.includes(ext)) max = LIMITS.code;
      else if (ext === '.json') max = f.name === 'theme.json' ? LIMITS.themeJson : LIMITS.json;
      else if (IMAGE_EXT.includes(ext)) max = LIMITS.image;
      else if (ext === '.md' || ext === '.txt' || TEXT_NAMES.includes(base)) max = LIMITS.text;
      else { add('error', 'files.type', f.name, null, 'file type not allowed: code .js/.jsx, data .json, images .png/.jpg, text .md/.txt/LICENSE/NOTICE'); continue; }
      if (f.size > max) add('error', 'files.size', f.name, null, `${f.size} bytes; the limit for this kind of file is ${max}`);
      if (IMAGE_EXT.includes(ext) && !f.name.startsWith('assets/') && !f.name.startsWith('screenshots/')) add('warning', 'files.image', f.name, null, 'only images in assets/ are installed');
    }
    if (count > LIMITS.files) add('error', 'files.count', null, null, `${count} files; at most ${LIMITS.files}`);
    if (total > LIMITS.total) add('error', 'files.total', null, null, `${total} bytes in all; at most ${LIMITS.total}`);
    for (const need of ['theme.json', 'index.jsx']) if (!names.has(need)) add('error', 'files.required', need, null, `${need} is required`);
    const shots = files.filter((f) => f.name.startsWith('screenshots/'));
    if (shots.length > LIMITS.screenshots) add('error', 'files.screenshots', 'screenshots/', null, `at most ${LIMITS.screenshots} screenshots`);
  }

  function checkManifest(text, id, add) {
    let m;
    try { m = JSON.parse(text); } catch (e) { add('error', 'manifest.json', 'theme.json', null, `not valid JSON: ${e.message}`); return null; }
    if (!m || typeof m !== 'object' || Array.isArray(m)) { add('error', 'manifest.object', 'theme.json', null, 'theme.json must be a JSON object'); return null; }
    const err = (f, msg) => add('error', `manifest.${f}`, 'theme.json', null, msg);
    const okText = (v, max) => typeof v === 'string' && !UNSAFE_TEXT.test(v) && v.trim() && [...v.trim()].length <= max;
    if (!okText(m.name, 48)) err('name', '"name": plain text on one line, at most 48 characters');
    if (!okText(m.description, 240)) err('description', '"description": one sentence of plain text');
    if (m.hidden === true) err('hidden', '"hidden": true keeps the theme out of Settings');
    if (m.community !== true) err('community', '"community": true is required for a theme LordOS didn\'t make');
    if (!okText(m.author, 80) || /^author pending$/i.test(String(m.author).trim())) err('author', '"author": who made it, plain text, at most 80 characters');
    if (typeof m.license !== 'string' || !LICENSE.test(m.license)) err('license', '"license": an SPDX expression such as "MIT"');
    if (typeof m.version !== 'string' || !VERSION_RE.test(m.version)) err('version', '"version": "1.0.0" style');
    if (m.capabilities !== undefined) {
      if (!Array.isArray(m.capabilities) || m.capabilities.length > CAPABILITIES.length) err('capabilities', `"capabilities": a list of ${CAPABILITIES.join(', ')}`);
      else for (const c of m.capabilities) if (!CAPABILITIES.includes(c)) err('capabilities', `"capabilities": "${String(c).slice(0, 40)}" is not one of ${CAPABILITIES.join(', ')}`);
    }
    if (m.minLordOS !== undefined && (typeof m.minLordOS !== 'string' || !/^[0-9]{1,5}(\.[0-9]{1,5}){1,5}$/.test(m.minLordOS))) err('minLordOS', '"minLordOS": a LordOS version such as "0.8.9.6.2"');
    void id;
    return m;
  }

  function checkLocales(files, add) {
    const out = {};
    for (const f of files.filter((x) => x.name.startsWith('locales/'))) {
      const m = /^locales\/([A-Za-z-]+)\.json$/.exec(f.name);
      if (!m || !LANGUAGES.includes(m[1])) { add('error', 'i18n.file', f.name, null, 'locales/ holds <language>.json for LordOS\'s languages'); continue; }
      let c;
      try { c = JSON.parse(f.text); } catch (e) { add('error', 'i18n.json', f.name, null, `not valid JSON: ${e.message}`); continue; }
      if (!c || typeof c !== 'object' || Array.isArray(c) || Object.values(c).some((v) => typeof v !== 'string')) { add('error', 'i18n.flat', f.name, null, 'a flat object of "key": "text"'); continue; }
      for (const [k, v] of Object.entries(c)) {
        if (!/^[A-Za-z0-9_.-]{1,96}$/.test(k)) add('error', 'i18n.key', f.name, null, `key "${k.slice(0, 40)}": letters, digits, ".", "-", "_"`);
        if (/[‪-‮⁦-⁩]/.test(v)) add('error', 'i18n.bidi', f.name, null, `"${k}": bidi override characters are not allowed`);
      }
      out[m[1]] = c;
    }
    return out;
  }

  function checkCode(f, add, ownNs, catalogs) {
    const text = f.text;
    const rel = f.name;
    const r = scan(text);
    const m = r.masked;
    const lineOf = (pos) => { let l = 1; for (let k = 0; k < pos; k++) if (text.charCodeAt(k) === 10) l++; return l; };
    const err = (rule, line, msg) => add('error', rule, rel, line, msg);
    const warn = (rule, line, msg) => add('warning', rule, rel, line, msg);
    for (const e of r.errors) err('code.syntax', e.line, e.message);
    text.split('\n').forEach((ln, k) => { if (ln.length > LIMITS.line) err('code.minified', k + 1, `a ${ln.length}-character line: ship readable source`); });
    for (const s of r.strings) if (s.raw.length > LIMITS.stringLiteral) err('code.blob', s.line, 'a very long string literal: data belongs in a .json or an image');
    const escapes = (text.match(/\\x[0-9a-fA-F]{2}|\\u[0-9a-fA-F]{4}/g) || []).length;
    if (escapes > 40) warn('code.escapes', null, `${escapes} \\x/\\u escapes: write the characters themselves`);
    for (const mm of m.matchAll(/\b_0x[0-9a-f]{3,}\b/g)) { err('code.obfuscated', lineOf(mm.index), 'obfuscated identifiers (_0x…)'); break; }
    for (const mm of m.matchAll(/\bimport\s*\(/g)) err('code.dynamicImport', lineOf(mm.index), 'dynamic import() is not allowed');
    for (const mm of m.matchAll(/\brequire\s*\(/g)) err('code.require', lineOf(mm.index), 'require() is not allowed: use import');
    const forbid = [
      [/\beval\b/g, 'code.eval', 'eval is not allowed'],
      [/\bnew\s+Function\b|(?<![\w$.])Function\b/g, 'code.function', 'Function builds code from text: not allowed'],
      [/\bglobalThis\b|(?<![\w$.])(self|global|window|frames|parent|top)\s*(\.|\[)/g, 'code.globalThis', 'the global object is off limits: use what @lordos/sdk exports'],
      [/(?<![\w$])__[A-Za-z0-9_$]*/g, 'code.internal', 'names starting with "__" are LordOS\'s (its internals and the theme compiler\'s)'],
      [/\b__proto__\b|\bconstructor\b|\bprototype\s*\[|\bcaller\b|\bcallee\b|\b__defineGetter__\b|\b__lookupGetter__\b/g, 'code.prototype', 'prototype tricks are not allowed'],
      [/\bReflect\b|\bObject\s*\.\s*(setPrototypeOf|defineProperty|defineProperties|getOwnPropertyDescriptors?|getPrototypeOf|__proto__)\b|\bsetPrototypeOf\b/g, 'code.patch', 'reflection and patching objects are not allowed'],
      [/\b(fetch|XMLHttpRequest|WebSocket|EventSource|importScripts|Worker|SharedArrayBuffer|Atomics|WebAssembly)\b/g, 'code.network', 'a theme doesn\'t reach the network or run other code'],
      [/\bset(Timeout|Interval)\s*\(\s*['"`]/g, 'code.timerString', 'a timer with a string runs it as code: pass a function'],
      [/\bwith\s*\(/g, 'code.with', '"with" is not allowed'],
      [/\batob\s*\(|\bbtoa\s*\(|\bString\s*\.\s*fromCharCode\s*\(|\bString\s*\.\s*fromCodePoint\s*\(/g, 'code.decode', 'decoding hidden text looks like obfuscation: write it plainly'],
      [/\bSymbol\s*\.\s*for\b/g, 'code.symbol', 'Symbol.for reaches values shared with LordOS\'s code'],
      [/\bnew\s+Proxy\b|\bProxy\s*\./g, 'code.proxy', 'Proxy objects are not allowed in a community theme'],
    ];
    for (const [re, rule, msg] of forbid) for (const mm of m.matchAll(re)) err(rule, lineOf(mm.index), msg);
    // lord.* used directly
    for (const mm of m.matchAll(/(?<![\w$.])lord\s*(?:\.\s*([A-Za-z_$][\w$]*)|\[)/g)) {
      const ns = mm[1];
      const line = lineOf(mm.index);
      if (!ns) { err('lord.dynamic', line, 'lord[…] with a computed name is not allowed'); continue; }
      if (LORD_UI.has(ns)) continue;
      if (LORD_FORBIDDEN[ns]) err('lord.forbidden', line, `lord.${ns}: ${LORD_FORBIDDEN[ns]} is out of a community theme's reach`);
      else err('lord.direct', line, `lord.${ns}: a community theme's lord has only ${[...LORD_UI].join(', ')}; use the SDK`);
    }
    for (const mm of m.matchAll(/(?<![\w$.]|typeof\s)lord(?!\s*[.[])\b/g)) { err('lord.alias', lineOf(mm.index), 'don\'t pass the lord object around'); break; }
    for (const s of r.strings) {
      if (/^[a-z][a-z0-9+.-]{1,15}:\/\//i.test(s.value.trim())) err('code.url', s.line, 'no remote URLs in a theme');
      else if (/^data:/i.test(s.value)) warn('code.dataUri', s.line, 'a data: URI: put the image in assets/');
      if ((s.attr === 'src' || /^(\.\.\/|\/)/.test(s.value)) && (/(^|\/)\.\.(\/|$)/.test(s.value) || (s.attr === 'src' && s.value.startsWith('/')))) {
        err('code.path', s.line, 'images come from assets/ (or LordOS\'s "asset:" pictures), never outside the theme');
      }
      if (!WORDS.test(s.value) || /^[a-z][\w-]*(\.[\w-]+)+$/.test(s.value) || s.dynamic) continue;
      if (/^[A-Z0-9]{2,5}$/.test(s.value) || SHORTCUT.test(s.value)) continue;
      const before = m.slice(Math.max(0, s.pos - 40), s.pos);
      if (/(?<![\w$.])t(Optional)?\s*\(\s*$/.test(before)) continue;
      if (s.kind === 'jsxattr' && VISIBLE_PROPS.test(s.attr || '')) err('i18n.literal', s.line, `${s.attr}="${s.value.slice(0, 40)}": visible text goes through t('…')`);
    }
    for (const x of r.jsxText) if (WORDS.test(x.value)) err('i18n.literal', x.line, `"${x.value.slice(0, 50)}": visible text goes through t('…')`);
    // keys of the theme's own namespace exist in English
    for (const mm of m.matchAll(/(?<![\w$.])t(?:Optional)?\s*\(\s*(['"`])/g)) {
      const s = r.strings.find((x) => x.pos === mm.index + mm[0].length - 1);
      if (!s || s.dynamic || !s.value.startsWith(`${ownNs}.`)) continue;
      const k = s.value.slice(ownNs.length + 1);
      if (!catalogs.en || !(k in catalogs.en)) err('i18n.unknown', s.line, `t('${s.value}'): "${k}" is not in locales/en.json`);
    }
  }

  // ------------------------------------------------------------------------------------------------------------------
  // compile

  const RUNTIME = [
    '(function () {',
    '"use strict";',
    'const __host = __lordThemeSdk;',
    'const __h = __host.react.createElement, __Fragment = __host.react.Fragment;',
    'const __hostMods = {',
    '  "react": Object.freeze(Object.assign({ default: __host.react }, __host.react)),',
    '  "@lordos/sdk": Object.freeze(Object.assign({ default: __host.sdk }, __host.sdk)),',
    '  "@lordos/apps": Object.freeze({ apps: __host.apps, default: { apps: __host.apps } }),',
    '};',
    'const __mods = new Map(), __done = new Map();',
    'const __def = (n, f) => { __mods.set(n, f); };',
    'const __load = (n) => {',
    '  if (Object.prototype.hasOwnProperty.call(__hostMods, n)) return __hostMods[n];',
    '  if (__done.has(n)) return __done.get(n);',
    '  const e = {};',
    '  __done.set(n, e);',
    '  __mods.get(n)(__load, e);',
    '  return e;',
    '};',
  ].join('\n');

  function resolver(names) {
    return (spec, from, line) => {
      if (HOST_MODULES.includes(spec)) return spec;
      if (!spec.startsWith('./') && !spec.startsWith('../')) throw new BuildError(line, `"${spec}": a theme imports only ${HOST_MODULES.join(', ')} and its own files`);
      const parts = (dirOf(from) ? dirOf(from).split('/') : []);
      for (const p of spec.split('/')) {
        if (p === '.' || p === '') continue;
        if (p === '..') { if (!parts.length) throw new BuildError(line, `"${spec}" points outside the theme`); parts.pop(); } else parts.push(p);
      }
      const t = parts.join('/');
      for (const c of [t, `${t}.js`, `${t}.jsx`, `${t}/index.js`, `${t}/index.jsx`]) if (names.has(c)) return c;
      throw new BuildError(line, `"${spec}" is not a file in the theme`);
    };
  }

  // opts.checks === false: the build only (server/tests' fixtures of what the checks refuse, to test what lord-server
  // does with such code anyway); lord-themec never passes it
  function compile(input, opts) {
    const checks = !(opts && opts.checks === false);
    const findings = [];
    const add = (level, rule, file, line, message) => { if (findings.length < 300) findings.push({ level, rule, file: file || null, line: line || null, message: String(message) }); };
    const id = String(input && input.id || '');
    const files = Array.isArray(input && input.files) ? input.files.filter((f) => f && typeof f.name === 'string') : [];
    if (!THEME_ID.test(id)) add('error', 'id', null, null, 'the archive\'s folder is the theme\'s id: letters, digits, - and _, at most 26 characters');
    if (checks) checkFiles(files, add);
    const byName = new Map(files.map((f) => [f.name, f]));
    const ownNs = `theme-${id}`;
    const metaFile = byName.get('theme.json');
    const meta = metaFile && typeof metaFile.text === 'string' ? checkManifest(metaFile.text, id, checks ? add : () => {}) : null;
    const catalogs = checkLocales(files, checks ? add : () => {});
    if (files.some((f) => f.name.startsWith('locales/')) && !catalogs.en) add('error', 'i18n.en', 'locales/en.json', null, 'English (locales/en.json) is required');
    const codeFiles = files.filter((f) => CODE_EXT.includes(extOf(f.name)) && typeof f.text === 'string');
    if (checks) for (const f of codeFiles) checkCode(f, add, ownNs, catalogs);
    const index = byName.get('index.jsx');
    if (checks && index && typeof index.text === 'string' && !/\bstartShell\s*\(/.test(scan(index.text).masked)) add('error', 'shell.startShell', 'index.jsx', null, 'index.jsx must end with startShell(<App />)');

    // the build, from index.jsx
    let code = null;
    if (!findings.some((x) => x.level === 'error')) {
      const names = new Set(files.map((f) => f.name));
      const resolve = resolver(names);
      const parts = [RUNTIME];
      const seen = new Set();
      const queue = ['index.jsx'];
      try {
        while (queue.length) {
          const name = queue.shift();
          if (seen.has(name) || HOST_MODULES.includes(name)) continue;
          seen.add(name);
          const f = byName.get(name);
          if (extOf(name) === '.json') {
            let v;
            try { v = JSON.parse(f.text); } catch (e) { throw new BuildError(null, `${name}: not valid JSON`); }
            parts.push(`__def(${JSON.stringify(name)}, (__require, __exports) => { __exports.default = ${JSON.stringify(v)}; });`);
            continue;
          }
          let t;
          try { t = transform(f.text, name, resolve); } catch (e) {
            if (e instanceof BuildError) { add('error', 'build', name, e.line, e.message); continue; }
            throw e;
          }
          parts.push(`__def(${JSON.stringify(name)}, (__require, __exports) => {\n"use strict";\n${t.code}\n});`);
          for (const d of t.deps) if (!seen.has(d)) queue.push(d);
        }
      } catch (e) {
        add('error', 'build', null, e.line || null, e.message);
      }
      parts.push('__load("index.jsx");', '})();', '');
      if (!findings.some((x) => x.level === 'error')) {
        code = parts.join('\n');
        if (code.length > LIMITS.out) { add('error', 'build.size', null, null, 'the built code is too large'); code = null; }
      }
    }
    const errors = findings.filter((x) => x.level === 'error').length;
    const warnings = findings.filter((x) => x.level === 'warning').length;
    const keep = errors ? [] : files.map((f) => f.name).filter((nm) => nm === 'theme.json' || (nm.startsWith('assets/') && IMAGE_EXT.includes(extOf(nm))) ||
      /^locales\/[A-Za-z-]+\.json$/.test(nm) || ['LICENSE', 'NOTICE', 'THIRD_PARTY.md', 'README.md'].includes(nm));
    const caps = meta && Array.isArray(meta.capabilities) ? meta.capabilities.filter((c) => CAPABILITIES.includes(c)) : [];
    return {
      report: {
        ok: !errors, id, errors, warnings, findings, compiler: VERSION,
        meta: meta ? { name: String(meta.name || '').slice(0, 48), author: String(meta.author || '').slice(0, 80), version: String(meta.version || '').slice(0, 32),
          license: String(meta.license || '').slice(0, 120), capabilities: caps } : null,
      },
      code: errors ? null : code,
      keep,
    };
  }

  G.LordThemec = Object.freeze({ compile, transform: (src, file, names) => transform(src, file, resolver(new Set(names || []))), scan, VERSION, CAPABILITIES, LIMITS });
})(globalThis);
