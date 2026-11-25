// links.js — what a note's links and images may reach (GitHub #238), pure so tests/notes_store_test.mjs checks it.
// A note is markdown anyone can write by hand, copy from somewhere or sync from another computer, so its targets are
// untrusted: only the note's own attachments are opened or drawn, by their plain file name inside the note's folder.
//
//   linkTarget(url)  → { kind: 'web', url }       an http(s) address (shown; never fetched or opened by Notes itself)
//                      { kind: 'file', name }     an attachment: one file name, no folders
//                      { kind: 'blocked' }        anything else: another scheme (file:, javascript:, data:, mailto:…),
//                                                 an absolute path, ~, a folder or a way out (.., /), control characters
//   attachmentRef(name)   the markdown target for a file name (spaces, parentheses and % encoded, so the link parses)
//   safeAttachName(name)  a file name fit for a note's folder: no slashes, control or direction-override characters
//   textual(name), imageKind(name)   how an attachment opens and whether it is drawn in the note

const CONTROL = /[\u0000-\u001f\u007f-\u009f]/;
// direction overrides and isolates: "gpj.exe" shown as "exe.jpg" (the classic spoof); zero-width joiners stay (emoji)
const BIDI = /[\u202a-\u202e\u2066-\u2069\u200e\u200f\u061c]/g;

export function linkTarget(url) {
  const raw = String(url == null ? '' : url).trim();
  if (!raw || raw.length > 1024 || CONTROL.test(raw)) return { kind: 'blocked' };
  if (/^https?:\/\/[^\s/]/i.test(raw)) return { kind: 'web', url: raw };
  // any other scheme (a letter, then letters, digits, + - . and a colon), and protocol-relative //host
  if (/^[a-z][a-z0-9+.-]*:/i.test(raw) || raw.startsWith('//')) return { kind: 'blocked' };
  let name = raw;
  try { name = decodeURIComponent(raw); } catch (e) { name = raw; }
  if (CONTROL.test(name)) return { kind: 'blocked' };
  if (name.startsWith('./')) name = name.slice(2);
  if (!name || name === '.' || name === '..' || name.startsWith('~') || /[/\\]/.test(name)) return { kind: 'blocked' };
  if (name.length > 255) return { kind: 'blocked' };
  return { kind: 'file', name };
}

export function safeAttachName(name) {
  let s = String(name == null ? '' : name).replace(BIDI, '').replace(/[\u0000-\u001f\u007f-\u009f]/g, '_').replace(/[/\\]/g, '_').trim();
  if (!s || s === '.' || s === '..') s = 'attachment';
  // a name a link can't confuse with something else (~, a leading dot-dot)
  if (s.startsWith('~')) s = `_${s.slice(1)}`;
  // at most 200 UTF-16 units, the extension kept
  if (s.length > 200) {
    const dot = s.lastIndexOf('.');
    const ext = dot > 0 && s.length - dot <= 16 ? s.slice(dot) : '';
    s = s.slice(0, 200 - ext.length) + ext;
  }
  return s;
}

// what goes between the parentheses: the characters that would end or break the link encoded
export const attachmentRef = (name) => String(name).replace(/[%\s()<>[\]]/g, (c) => `%${c.charCodeAt(0).toString(16).toUpperCase().padStart(2, '0')}`);
// what goes between the brackets: a bracket would end the label (the inline parser takes no escapes there), so
// brackets become parentheses; a backslash is doubled (the parser reads a backslash and x as x)
export const linkLabel = (name) => String(name).replace(/\[/g, '(').replace(/\]/g, ')').replace(/\\/g, '\\\\');

const TEXT = /\.(txt|md|markdown|csv|log|json|xml|ya?ml|toml|ini|c|h|cc|cpp|hpp|js|jsx|ts|tsx|py|sh|css|html?)$/i;
export const textual = (name) => TEXT.test(String(name));

// The image formats lord-server draws itself (server/src/ui/stb.c: stb_image with PNG and JPEG only). GIF, WebP, BMP,
// HEIC, HEIF, AVIF and TIFF are images Photos opens (apps/photos/index.jsx IMAGE): they stay attachments in the note
// and open there.
const DRAWN = /\.(png|jpe?g)$/i;
const PHOTO = /\.(gif|webp|bmp|heic|heif|avif|tiff?)$/i;
export const imageKind = (name) => (DRAWN.test(String(name)) ? 'inline' : PHOTO.test(String(name)) ? 'photo' : null);
export const isImage = (name) => imageKind(name) === 'inline';
