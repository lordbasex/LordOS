// paths.js — files into the Terminal as text (pure; server/tests/terminal_paths_test.mjs): a file dropped from
// Files, or the image on the clipboard, goes in as its path, quoted for the shell and followed by a space, as
// other terminals do. Claude Code and similar tools take a pasted image path as an attached image.

// One argument for a POSIX shell: plain when only safe characters, else in single quotes.
export function shellQuote(p) {
  const s = String(p);
  return /^[A-Za-z0-9_@%+=:,./-]+$/.test(s) ? s : `'${s.replace(/'/g, "'\\''")}'`;
}

export const pathsText = (paths) => (paths || []).filter(Boolean).map(shellQuote).join(' ') + ' ';

// The clipboard's image file (lord.clipboard.state(): the compositor keeps it as a PNG under
// $XDG_RUNTIME_DIR/lordos), or null when the clipboard holds text.
export const clipboardImage = (st) => (st && st.kind === 'image' && st.image ? st.image : null);

// Programs the status bar names by what they are rather than by their process name.
const PROGRAMS = { claude: 'Claude Code' };
export const programLabel = (process) => PROGRAMS[process] || process || '';
export const isClaude = (process) => process === 'claude';
