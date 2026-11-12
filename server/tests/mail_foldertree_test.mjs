// Mail's sidebar folder tree (issue #28), from server/: node tests/mail_foldertree_test.mjs
import assert from 'node:assert/strict';
import { folderTree, toggleCollapsed, accountUnread } from '../../apps/mail/foldertree.js';

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

// collapsing (issue #51): triangles only on folders with visible subfolders; a closed one hides
// everything under it and carries the hidden folders, so its unread mail can still be counted
const kids = (tree) => tree.map(({ f, kids: k, open }) => `${f.special || f.display}:${k}${open ? '' : '+'}`);
assert.deepEqual(kids(folderTree(dot)), ['inbox:0', 'drafts:0', 'sent:0', 'junk:0', 'trash:1', 'Old:0', 'Family:0', 'Sent Messages:0', 'Work:1', 'Clients:1', 'Acme:0']);
const shut = folderTree(dot, 8, ['INBOX.Work']);
assert.deepEqual(show(shut), ['inbox', 'drafts', 'sent', 'junk', 'trash', '  Old', 'Family', 'Sent Messages', 'Work']);
assert.deepEqual(shut.find((x) => x.f.name === 'INBOX.Work').hidden.map((f) => f.name), ['INBOX.Work.Clients', 'INBOX.Work.Clients.Acme']);
assert.equal(shut.find((x) => x.f.name === 'INBOX.Work').open, false);
// closing a middle level keeps its parent open; a Set works too; a closed leaf changes nothing
assert.deepEqual(show(folderTree(dot, 8, new Set(['INBOX.Work.Clients']))).slice(-2), ['Work', '  Clients']);
assert.deepEqual(show(folderTree(dot, 8, ['INBOX.Family', 'nonsense'])), show(folderTree(dot)));
assert.equal(folderTree(dot, 8, ['INBOX.Family']).find((x) => x.f.name === 'INBOX.Family').open, true);
// a \Noselect container counts only its visible kids; the hidden Gmail one gets no triangle at all
assert.deepEqual(kids(folderTree(gmail)), ['inbox:0', 'sent:0', 'trash:0', 'Lists:1', 'Dev:0']);
assert.deepEqual(show(folderTree(gmail, 8, ['Lists'])), ['inbox', 'sent', 'trash', 'Lists']);
// garbage in the saved list is ignored
assert.deepEqual(show(folderTree(dot, 8, null)), show(folderTree(dot)));
assert.deepEqual(show(folderTree(dot, 8, 'INBOX.Work')), show(folderTree(dot)));

// the saved list: toggled, sorted, without folders that are gone, never a name the server does not list
assert.deepEqual(toggleCollapsed([], 'INBOX.Work', dot), ['INBOX.Work']);
assert.deepEqual(toggleCollapsed(['INBOX.Work'], 'INBOX.Work', dot), []);
assert.deepEqual(toggleCollapsed(['INBOX.Work', 'Old/Gone', 7, null], 'INBOX.Trash', dot), ['INBOX.Trash', 'INBOX.Work']);
assert.deepEqual(toggleCollapsed(undefined, 'Nope', dot), []);
assert.deepEqual(toggleCollapsed(['INBOX.Work', 'INBOX.Work'], 'INBOX.Family', dot), ['INBOX.Family', 'INBOX.Work']);

// a folded account's header (#121): unread mail in every selectable folder but Sent, Drafts, Junk and Trash
const U = (f, unread, total = 0) => ({ ...f, unread, total });
assert.equal(accountUnread([U(dot[0], 3), U(dot[1], 0, 9), U(dot[2], 4), U(dot[3], 7), U(dot[4], 2), U(dot[5], 5), U(dot[8], 1)]), 9);
assert.equal(accountUnread([U(gmail[3], 6), U(gmail[5], 2)]), 2);           // a \Noselect container counts nothing
assert.equal(accountUnread([U(dot[0], -1), U(dot[5], 'x'), U(dot[6], 2.5), null]), 2);
assert.equal(accountUnread(undefined), 0);

console.log('mail-foldertree-test: ok');
