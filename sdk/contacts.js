// contacts.js — Contacts (docs/MAIL.md): address books kept by axia-maild (local, or synced
// with CardDAV). Mail autocompletes addresses from here and from the people you write to.
//
//   books.list() → [{ id, name, color, acct, acctName, kind: 'local'|'dav', count }]
//   books.create({ name }) / update(id, { name }) / remove(id)
//   contacts.list(book?) → [{ uid, book, fn, n: { family, given, middle, prefix, suffix }, nick, org, dept, title,
//        emails: [{ type, value }], phones, urls, im, addrs: [{ type, street, city, region, code, country }],
//        bday ('YYYY-MM-DD'), notes, cats: [], photo: bool, kind: 'individual'|'group', members: [uid] }]
//   contacts.save(book, contact, { photo: 'keep' | 'remove' | path }) → uid
//   contacts.remove(book, uid) / importFile(path, book)
//   contacts.photo(book, uid, size) → image handle for <ContactPhoto> (decoded in axia-maild)
//   contacts.autocomplete(q) → [{ name, email, src: 'contacts'|'mail' }]
//   displayName(c) / initials(c)
//   <ContactPhoto contact size />            the photo or the initials on a colored circle
import { createElement as h, useEffect, useRef, useState } from 'react';
import { View, Text, Image } from '@lordos/runtime';
import { mailRequest } from './mail.js';
import { usePim } from './calendar.js';

export const books = {
  list: async () => (await mailRequest('book-list')).books || [],
  create: (b) => mailRequest('book-create', b),
  update: (id, patch) => mailRequest('book-update', { id, ...patch }),
  remove: (id) => mailRequest('book-delete', { id }),
};

export const contacts = {
  list: async (book) => (await mailRequest('contact-list', book ? { book } : {})).contacts || [],
  async save(book, contact, { photo = 'keep' } = {}) {
    const isPath = photo && photo !== 'keep' && photo !== 'remove';
    const r = await mailRequest('contact-save', { book, contact, photo: isPath ? 'new' : photo }, isPath ? { files: [photo] } : {});
    return r.uid;
  },
  remove: (book, uid) => mailRequest('contact-delete', { book, uid }),
  importFile: (path, book) => mailRequest('contact-import', { book }, { files: [path] }),
  photo: async (book, uid, size = 128) => (await mailRequest('contact-photo', { book, uid, size })).image,
  // Contacts come first; a person you wrote to who is also a contact appears once.
  autocomplete: async (q) => {
    const seen = new Set();
    return ((await mailRequest('autocomplete', { q })).items || []).filter((x) => {
      const k = (x.email || '').toLowerCase();
      if (!k || seen.has(k)) return false;
      seen.add(k);
      return true;
    });
  },
};

export function displayName(c) {
  if (!c) return '';
  if (c.fn) return c.fn;
  const n = c.n || {};
  const full = [n.prefix, n.given, n.middle, n.family, n.suffix].filter(Boolean).join(' ');
  return full || c.org || c.emails?.[0]?.value || '';
}

export function initials(c) {
  const n = c?.n || {};
  const a = (n.given || '').trim()[0] || '', b = (n.family || '').trim()[0] || '';
  if (a || b) return (a + b).toUpperCase();
  const d = displayName(c).trim();
  return d ? [...d][0].toUpperCase() : '?';
}

const PALETTE = ['#ff6b6b', '#f7a440', '#e0b400', '#4cb860', '#36a3d9', '#6c7ae0', '#b06ad9', '#e05f9b', '#8e8e93'];
const colorOf = (s) => { let x = 0; for (const ch of String(s || '')) x = (x * 31 + ch.codePointAt(0)) >>> 0; return PALETTE[x % PALETTE.length]; };

export function ContactPhoto({ contact, size = 40 }) {
  const rev = usePim();
  const ref = useRef(null);
  const [handle, setHandle] = useState(0);
  const want = contact?.photo && contact.book && contact.uid;
  useEffect(() => {
    let live = true;
    if (!want) { setHandle(0); return undefined; }
    contacts.photo(contact.book, contact.uid, Math.min(512, Math.round(size * 2))).then((img) => { if (live) setHandle(img || 0); }).catch(() => { if (live) setHandle(0); });
    return () => { live = false; };
  }, [contact?.uid, contact?.book, want, rev, size]);
  useEffect(() => {
    if (ref.current && handle) lord.mail.bind(ref.current.id, handle);
    return () => { if (handle) lord.mail.release(handle); };
  }, [handle]);
  const box = { width: size, height: size, borderRadius: size / 2, overflow: 'hidden' };
  if (want && handle) return h(View, { style: box }, h(Image, { ref, style: { width: size, height: size, objectFit: 'cover' } }));
  return h(View, { style: { ...box, backgroundColor: colorOf(displayName(contact)), alignItems: 'center', justifyContent: 'center' } },
    h(Text, { style: { color: '#fff', fontSize: size * 0.4, fontWeight: 600 } }, initials(contact)));
}
