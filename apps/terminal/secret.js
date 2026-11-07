// secret.js — Passwords in the Terminal, the pure part (SecretBar.jsx; server/tests/terminal_secret_test.mjs):
// the host a password prompt is for, and the Passwords entries to offer, those for that host first.

// "user@host's password:" (ssh), "Password for 'https://user@host':" (git), "Enter passphrase for key '…'"
// (no host). The session's own host (OSC 7 / OSC 1337, "user@host") when the prompt names none: a
// password asked inside an ssh session is most likely that machine's.
export function secretHost(prompt, sessionHost) {
  const p = String(prompt || '');
  let m = /([A-Za-z0-9._-]+)@([A-Za-z0-9.-]+)'s password/.exec(p);
  if (m) return m[2].toLowerCase();
  m = /https?:\/\/(?:[^@/'"\s]+@)?([A-Za-z0-9.-]+)/.exec(p);
  if (m) return m[1].toLowerCase();
  m = /@([A-Za-z0-9.-]+)\s*$/.exec(String(sessionHost || ''));
  return m && m[1] !== 'localhost' ? m[1].toLowerCase() : '';
}

const hostOf = (url) => {
  const m = /^(?:[a-z][a-z0-9+.-]*:\/\/)?(?:[^@/]+@)?([^/:?#]+)/i.exec(String(url || '').trim());
  return m ? m[1].toLowerCase() : '';
};

// Logins (category "internet") not in the trash, those whose address or title names the host first
// (marked match), then the rest by title.
export function rankEntries(entries, host) {
  const h = String(host || '').toLowerCase();
  const first = h.split('.')[0];
  const isMatch = (e) => {
    if (!h) return false;
    const u = hostOf(e.url);
    return (u && (u === h || u.endsWith(`.${h}`) || h.endsWith(`.${u}`))) || (first.length > 2 && String(e.title || '').toLowerCase().includes(first));
  };
  return (entries || [])
    .filter((e) => e && !e.trash && e.cat === 'internet')
    .map((e) => ({ id: e.id, title: e.title || '', sub: e.sub || '', match: isMatch(e) }))
    .sort((a, b) => (b.match - a.match) || a.title.localeCompare(b.title));
}
