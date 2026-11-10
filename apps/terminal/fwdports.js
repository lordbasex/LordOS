// fwdports.js — a forward whose fixed local port is already taken (pure; server/tests/terminal_fwdports_test.mjs).
// lord.sftp's forward answers "inuse" when something listens on 127.0.0.1:<local> (sftpc.c port_taken): another tab's
// ssh with the same profile, or another program. forwards.js tells the person once per session and port, and "Use a
// Free Port" opens it on an automatic one for the rest of that session (its next connections too).

// a forward as the session remembers the choice: server host and port, and the fixed local port it asked for
export const fwdKey = (it) => `${it.host}:${it.port}:${it.want}`;

// the profile's forwards, the ones moved to a free port in this session made automatic
export const applyFree = (list, free) => list.map((f) => (f.want && free?.has(fwdKey(f)) ? { ...f, want: 0 } : f));

// the local ports refused as taken and not told yet (in order, once each)
export function takenPorts(items, told) {
  const out = [];
  for (const it of items || []) if (it.state === 'error' && it.error === 'inuse' && it.want && !told?.has(it.want) && !out.includes(it.want)) out.push(it.want);
  return out;
}

// the badge's forward: an open one, else one whose port is taken (it needs the person), else the first
export const badgeItem = (items) => items.find((x) => x.state === 'open') || items.find((x) => x.error === 'inuse') || items[0];
// the badge's text: "⇄ 80→54321", "⇄ 80→8080 !" when it failed, "+2" for the others
export function badgeText(items) {
  const it = badgeItem(items);
  return `⇄ ${it.port}→${it.local || it.want || '…'}${it.state === 'error' ? ' !' : ''}${items.length > 1 ? ` +${items.length - 1}` : ''}`;
}
