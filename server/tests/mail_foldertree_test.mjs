// Mail's sidebar folder tree (issue #28), from server/: node tests/mail_foldertree_test.mjs
import assert from 'node:assert/strict';
import { folderTree } from '../../apps/mail/foldertree.js';

const F = (name, special = '', delim = '.', extra = {}) => ({ name, display: delim && name.includes(delim) ? name.slice(name.lastIndexOf(delim) + 1) : name, special, delim, noselect: false, ...extra });
const show = (tree) => tree.map(({ f, depth }) => `${'  '.repeat(depth)}${f.special || f.display}`);

// Dovecot/Courier style: everything under "INBOX.", specials first (as lord-maild sorts them)
const dot = [
  F('INBOX', 'inbox'), F('INBOX.Drafts', 'drafts'), F('INBOX.Sent', 'sent'), F('INBOX.Junk', 'junk'), F('INBOX.Trash', 'trash'),
  F('INBOX.Work'), F('INBOX.Family'), F('INBOX.Work.Clients'), F('INBOX.Work.Clients.Acme'), F('INBOX.Trash.Old'), F('INBOX.Sent Messages'),
];
assert.deepEqual(show(folderTree(dot)), [
  'inbox', 'drafts', 'sent', 'junk', 'trash', '  Old',
  'Family', 'Sent Messages', 'Work', '  Clients', '    Acme',
]);

// "/" delimiter, folders at the top level, a child of INBOX, a missing middle level
const slash = [
  F('INBOX', 'inbox', '/'), F('Sent', 'sent', '/'), F('Trash', 'trash', '/'),
  F('INBOX/Receipts', '', '/'), F('Projects', '', '/'), F('Projects/2026', '', '/'), F('Archive2/Old/Deep', '', '/'), F('alpha', '', '/'),
];
assert.deepEqual(show(folderTree(slash)), [
  'inbox', '  Receipts', 'sent', 'trash', 'alpha', 'Deep', 'Projects', '  2026',
]);

// Gmail: a \Noselect container with only special folders inside is hidden; one with a user folder shows
const gmail = [
  F('INBOX', 'inbox', '/'), F('[Gmail]/Sent Mail', 'sent', '/'), F('[Gmail]/Trash', 'trash', '/'),
  F('[Gmail]', '', '/', { noselect: true }), F('Lists', '', '/', { noselect: true }), F('Lists/Dev', '', '/'),
];
assert.deepEqual(show(folderTree(gmail)), ['inbox', 'sent', 'trash', 'Lists', '  Dev']);
assert.equal(folderTree(gmail).find((x) => x.f.name === 'Lists').f.noselect, true);

// no delimiter (NIL), same name twice, junk input
assert.deepEqual(show(folderTree([F('INBOX', 'inbox', ''), F('a.b', '', ''), F('a.b', '', '')])), ['inbox', 'a.b']);
assert.deepEqual(folderTree(null), []);
assert.deepEqual(folderTree([null, {}, { name: '' }]), []);

// depth is capped
const deep = [F('INBOX', 'inbox', '/'), ...Array.from({ length: 12 }, (_, i) => F(Array.from({ length: i + 1 }, (_, k) => `L${k}`).join('/'), '', '/'))];
assert.equal(Math.max(...folderTree(deep, 8).map((x) => x.depth)), 8);

console.log('mail-foldertree-test: ok');
