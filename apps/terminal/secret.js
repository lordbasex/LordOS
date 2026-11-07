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

// The categories whose entries have a password a terminal program can ask for.
export const LOGIN_CATS = ['internet', 'ssh', 'database', 'network', 'general'];

// Whether an entry is for this host: its address, or its title naming the whole host ("ssh ana@10.0.0.5", an
// IP included) or its first label when that is a name ("github" in "GitHub").
export function entryMatches(e, host) {
  const h = String(host || '').toLowerCase();
  if (!h || !e) return false;
  const u = hostOf(e.url);
  if (u && (u === h || u.endsWith(`.${h}`) || h.endsWith(`.${u}`))) return true;
  const title = String(e.title || '').toLowerCase();
  if (title.includes(h)) return true;
  const first = h.split('.')[0];
  return !/^\d+$/.test(first) && first.length > 2 && title.includes(first);
}

// Logins not in the trash, those for the host first (marked match), then the rest by title.
export function rankEntries(entries, host) {
  return (entries || [])
    .filter((e) => e && !e.trash && LOGIN_CATS.includes(e.cat))
    .map((e) => ({ id: e.id, title: e.title || '', sub: e.sub || '', match: entryMatches(e, host) }))
    .sort((a, b) => (b.match - a.match) || a.title.localeCompare(b.title));
}

// "Save to Passwords?" (the "save" event: { token, kind, program, host, user, port }): not offered when Passwords
// already has an entry for that host and user (it was most likely filled from it, or is kept there already).
export function alreadySaved(entries, offer) {
  if (!offer || !offer.host) return false;
  const user = String(offer.user || '').toLowerCase();
  return (entries || []).some((e) => e && !e.trash && LOGIN_CATS.includes(e.cat) && entryMatches(e, offer.host)
    && (!user || String(e.sub || '').toLowerCase() === user || String(e.title || '').toLowerCase().includes(`${user}@`)));
}

// How an offer is shown: "ssh ec2-user@203.0.113.7".
export const offerTitle = (o) => `${o.program || ''} ${o.user ? `${o.user}@` : ''}${o.host || ''}`.trim();
