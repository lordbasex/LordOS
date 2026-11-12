// filekinds.js — each file's kind (by extension), its MIME type and whether it can be previewed.
import { splitVolume } from './archivevol.js';

const EXT = {
  image: ['png', 'jpg', 'jpeg', 'gif', 'webp', 'bmp', 'ico', 'svg', 'tif', 'tiff', 'heic', 'avif'],
  text: ['txt', 'md', 'markdown', 'rst', 'log', 'csv', 'tsv', 'nfo', 'conf', 'cfg', 'ini', 'toml', 'yaml', 'yml', 'env', 'srt', 'tex'],
  code: ['js', 'jsx', 'mjs', 'cjs', 'ts', 'tsx', 'json', 'c', 'h', 'cc', 'cpp', 'hpp', 'py', 'go', 'rs', 'sh', 'bash', 'zsh', 'fish',
    'html', 'htm', 'css', 'scss', 'php', 'rb', 'java', 'kt', 'swift', 'lua', 'pl', 'sql', 'xml', 'diff', 'patch', 'mk', 'cmake', 'vue', 'svelte', 'dart', 'zig', 'nim', 'r'],
  pdf: ['pdf'],
  archive: ['zip', 'gz', 'tgz', 'xz', 'bz2', 'zst', '7z', 'rar', 'tar', 'iso', 'img', 'dmg', 'lz', 'lzma'],
  audio: ['mp3', 'ogg', 'oga', 'opus', 'flac', 'wav', 'm4a', 'aac', 'wma', 'mid', 'midi'],
  video: ['mp4', 'mkv', 'webm', 'avi', 'mov', 'm4v', 'wmv', 'flv', 'mpg', 'mpeg', 'ogv'],
  lpk: ['lpk'],
  deb: ['deb'],
  doc: ['odt', 'doc', 'docx', 'rtf', 'ods', 'xls', 'xlsx', 'odp', 'ppt', 'pptx', 'epub'],
};
const BY_EXT = new Map();
for (const [kind, list] of Object.entries(EXT)) for (const e of list) BY_EXT.set(e, kind);

// Extensionless names that are text (Makefile, README…).
const TEXT_NAMES = /^(makefile|dockerfile|readme|license|copying|authors|changelog|todo|news|install|\.bashrc|\.profile|\.zshrc|\.gitignore|\.editorconfig)$/i;

export function extOf(name) {
  const m = /\.([^./]+)$/.exec(name);
  return m ? m[1].toLowerCase() : '';
}

export function kindOf(e) {
  if (e.app) return 'app';
  if (e.dir) return 'folder';
  const ext = extOf(e.name);
  // a volume of a split archive ("docs.zip.001", "docs.z01"): an archive too (GitHub #103)
  return BY_EXT.get(ext) || (TEXT_NAMES.test(e.name) ? 'text' : splitVolume(e.name) ? 'archive' : 'other');
}

// For "Open With" of Linux apps (their MimeType in the .desktop file). The names are
// shared-mime-info's; the system's own database (lord.mime.guess, see mimesOf) comes first, and this
// table is what is known without it. Every extension of a kind with no text fallback is here, so a
// kind never loses "Open With" for lack of a type (issue #49: the office documents had none).
const MIME = {
  png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif', webp: 'image/webp', svg: 'image/svg+xml', bmp: 'image/bmp',
  ico: 'image/vnd.microsoft.icon', tif: 'image/tiff', tiff: 'image/tiff', heic: 'image/heif', avif: 'image/avif',
  txt: 'text/plain', md: 'text/markdown', log: 'text/plain', csv: 'text/csv', html: 'text/html', htm: 'text/html', css: 'text/css',
  js: 'text/javascript', json: 'application/json', xml: 'application/xml', c: 'text/x-csrc', h: 'text/x-chdr', cpp: 'text/x-c++src',
  py: 'text/x-python', sh: 'application/x-shellscript', pdf: 'application/pdf', zip: 'application/zip', gz: 'application/gzip',
  tgz: 'application/x-compressed-tar', xz: 'application/x-xz', bz2: 'application/x-bzip2', zst: 'application/zstd',
  '7z': 'application/x-7z-compressed', rar: 'application/vnd.rar', tar: 'application/x-tar', iso: 'application/x-cd-image',
  img: 'application/x-raw-disk-image', dmg: 'application/x-apple-diskimage', lz: 'application/x-lzip', lzma: 'application/x-lzma',
  mp3: 'audio/mpeg', ogg: 'audio/ogg', oga: 'audio/ogg', opus: 'audio/x-opus+ogg', flac: 'audio/flac', wav: 'audio/x-wav',
  m4a: 'audio/mp4', aac: 'audio/aac', wma: 'audio/x-ms-wma', mid: 'audio/midi', midi: 'audio/midi',
  mp4: 'video/mp4', mkv: 'video/x-matroska', webm: 'video/webm', avi: 'video/x-msvideo', mov: 'video/quicktime', m4v: 'video/mp4',
  wmv: 'video/x-ms-wmv', flv: 'video/x-flv', mpg: 'video/mpeg', mpeg: 'video/mpeg', ogv: 'video/ogg',
  deb: 'application/vnd.debian.binary-package',
  odt: 'application/vnd.oasis.opendocument.text', ods: 'application/vnd.oasis.opendocument.spreadsheet',
  odp: 'application/vnd.oasis.opendocument.presentation', rtf: 'application/rtf', epub: 'application/epub+zip',
  doc: 'application/msword', docx: 'application/vnd.openxmlformats-officedocument.wordprocessingml.document',
  xls: 'application/vnd.ms-excel', xlsx: 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
  ppt: 'application/vnd.ms-powerpoint', pptx: 'application/vnd.openxmlformats-officedocument.presentationml.presentation',
  yaml: 'application/x-yaml', toml: 'application/toml',
};

export function mimeOf(e) {
  if (e.dir) return 'inode/directory';
  const ext = extOf(e.name);
  if (MIME[ext]) return MIME[ext];
  const k = kindOf(e);
  return k === 'text' || k === 'code' ? 'text/plain' : 'application/octet-stream';
}

// Every type an entry may be, best first: the system's MIME database's guess from the name
// (guess(name) → type or null: lord.mime.guess, /usr/share/mime/globs2), then mimeOf's. An app that
// declares either one opens it, so a type this table lacks still finds its apps through the system.
export function mimesOf(e, guess) {
  const out = [];
  if (!e.dir && typeof guess === 'function') {
    let g = null;
    try { g = guess(e.name); } catch (err) { g = null; }
    if (typeof g === 'string' && /^[\w.+-]+\/[\w.+-]+$/.test(g) && g !== 'application/octet-stream') out.push(g);
  }
  const own = mimeOf(e);
  if (!out.includes(own)) out.push(own);
  return out;
}

// Does the Linux app (mimeTypes "a/b;c/*;…") open this type?
export function appOpens(mimeTypes, mime) {
  if (!mimeTypes) return false;
  const [top] = mime.split('/');
  return mimeTypes.split(';').some((m) => m && (m === mime || m === `${top}/*` ||
    (mime.startsWith('text/') && m === 'text/plain')));
}

export const isTextual = (e) => ['text', 'code'].includes(kindOf(e));
export const isImage = (e) => kindOf(e) === 'image' && !/\.(svg|heic|avif|tiff?|webp)$/i.test(e.name);   // the ones stb_image draws (the rest: lord-media thumbnail)

// The src of a picture shown small (Files' icons and preview): "thumb:<px>:<path>", which lord-server decodes off the
// interface's thread straight down to fit px x px (server/src/ui/image.c). Until 0.8.8.7 an icon was the whole picture,
// decoded on that thread and kept for good: a folder of phone photos took 48 MB each and ran lord-server out of memory
// (issues #71, #82). px is the size shown, doubled for HiDPI and rounded up to 64, 128, … 2048 so near sizes share one.
export function thumbSrc(path, size) {
  const want = Math.ceil(Math.max(1, Number(size) || 0) * 2);
  let px = 64;
  while (px < want && px < 2048) px *= 2;
  return `thumb:${px}:${path}`;
}

// Band color of each kind in the document icon.
export const KIND_COLOR = {
  text: '#8e8e93', code: '#5e5ce6', pdf: '#ff3b30', archive: '#a2845e', audio: '#ff2d55', video: '#af52de',
  lpk: '#7d5bf0', deb: '#d70751', doc: '#0a84ff', image: '#30b0c7', other: '#8e8e93',
};
