// utf8.js — UTF-8 bytes ↔ text by hand: QuickJS, the desktop's JavaScript engine, has no TextEncoder or TextDecoder
// (a node test passes with them while the desktop throws). Tested by server/tests/utf8_test.mjs.

// text → bytes
export function utf8Encode(s) {
  const out = [];
  for (const ch of String(s)) {
    const c = ch.codePointAt(0);
    if (c < 0x80) out.push(c);
    else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
    else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
    else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
  }
  return new Uint8Array(out);
}

// how many bytes s takes in UTF-8, without building them
export function utf8Length(s) {
  let n = 0;
  for (const ch of String(s)) {
    const c = ch.codePointAt(0);
    n += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
  }
  return n;
}

// bytes → text; each maximal invalid subpart becomes one U+FFFD, as TextDecoder does
export function utf8Decode(bytes) {
  const b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes || []);
  let s = '';
  for (let i = 0; i < b.length;) {
    const c = b[i];
    if (c < 0x80) { s += String.fromCharCode(c); i++; continue; }
    const n = c >= 0xc2 && c < 0xe0 ? 1 : c >= 0xe0 && c < 0xf0 ? 2 : c >= 0xf0 && c < 0xf5 ? 3 : 0;
    if (!n) { s += '\ufffd'; i++; continue; }
    // the second byte's range depends on the first (no overlong forms, no surrogates, nothing past U+10FFFF)
    const lo = c === 0xe0 ? 0xa0 : c === 0xf0 ? 0x90 : 0x80, hi = c === 0xed ? 0x9f : c === 0xf4 ? 0x8f : 0xbf;
    let cp = c & (0x3f >> n), k = 1;
    for (; k <= n; k++) {
      const d = b[i + k];
      if (d === undefined || d < (k === 1 ? lo : 0x80) || d > (k === 1 ? hi : 0xbf)) break;
      cp = (cp << 6) | (d & 63);
    }
    if (k <= n) { s += '\ufffd'; i += k; continue; }
    s += String.fromCodePoint(cp);
    i += n + 1;
  }
  return s;
}
