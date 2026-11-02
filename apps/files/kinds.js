// kinds.js — each file's kind (by extension), its MIME type and whether it can be previewed.

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
  return BY_EXT.get(ext) || (TEXT_NAMES.test(e.name) ? 'text' : 'other');
}

// For "Open With" of Linux apps (their MimeType in the .desktop file).
const MIME = {
  png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif', webp: 'image/webp', svg: 'image/svg+xml', bmp: 'image/bmp',
  txt: 'text/plain', md: 'text/markdown', log: 'text/plain', csv: 'text/csv', html: 'text/html', htm: 'text/html', css: 'text/css',
  js: 'text/javascript', json: 'application/json', xml: 'application/xml', c: 'text/x-csrc', h: 'text/x-chdr', cpp: 'text/x-c++src',
  py: 'text/x-python', sh: 'application/x-shellscript', pdf: 'application/pdf', zip: 'application/zip', gz: 'application/gzip',
  mp3: 'audio/mpeg', ogg: 'audio/ogg', flac: 'audio/flac', wav: 'audio/x-wav', mp4: 'video/mp4', mkv: 'video/x-matroska',
  webm: 'video/webm', odt: 'application/vnd.oasis.opendocument.text', yaml: 'application/x-yaml', toml: 'application/toml',
};

export function mimeOf(e) {
  if (e.dir) return 'inode/directory';
  const ext = extOf(e.name);
  if (MIME[ext]) return MIME[ext];
  const k = kindOf(e);
  return k === 'text' || k === 'code' ? 'text/plain' : 'application/octet-stream';
}

// Does the Linux app (mimeTypes "a/b;c/*;…") open this type?
export function appOpens(mimeTypes, mime) {
  if (!mimeTypes) return false;
  const [top] = mime.split('/');
  return mimeTypes.split(';').some((m) => m && (m === mime || m === `${top}/*` ||
    (mime.startsWith('text/') && m === 'text/plain')));
}

export const isTextual = (e) => ['text', 'code'].includes(kindOf(e));
export const isImage = (e) => kindOf(e) === 'image' && !/\.(svg|heic|avif|tiff?|webp)$/i.test(e.name);   // the ones stb_image draws (the rest: axia-media thumbnail)

// Band color of each kind in the document icon.
export const KIND_COLOR = {
  text: '#8e8e93', code: '#5e5ce6', pdf: '#ff3b30', archive: '#a2845e', audio: '#ff2d55', video: '#af52de',
  lpk: '#7d5bf0', deb: '#d70751', doc: '#0a84ff', image: '#30b0c7', other: '#8e8e93',
};
