// templates.js — the built-in categories, each with its field template and its glyph (glyphs.jsx),
// and the field types. Names are translated when drawn (t('passwords.cat.<id>'),
// t('passwords.field.<key>')): that is why they are functions and not constants with text.
import { t } from '@lordos/sdk';
import { GLYPH_KINDS } from './glyphs.jsx';

// Fields of each template: k = key (stable, axia-vault uses it: "password" goes into the report,
// "totp" gives codes, "expiry" warns before it expires), t = type.
const F = (k, type = 'text', extra = {}) => ({ k, t: type, ...extra });

export const BUILTIN = [
  { id: 'general', icon: 'folder', color: '#8e8e93', fields: [F('username'), F('password', 'hidden'), F('url', 'url')] },
  { id: 'internet', icon: 'globe', color: '#0a84ff', fields: [F('url', 'url'), F('username'), F('password', 'hidden'), F('totp', 'totp')] },
  { id: 'email', icon: 'mail', color: '#5e5ce6', fields: [F('email', 'email'), F('password', 'hidden'), F('imap_host'), F('imap_port', 'number'),
    F('smtp_host'), F('smtp_port', 'number'), F('app_password', 'hidden')] },
  { id: 'finance', icon: 'bank', color: '#30d158', fields: [F('bank'), F('client'), F('username'), F('password', 'hidden'), F('transfer_key', 'hidden'),
    F('pin', 'pin'), F('coords', 'hidden', { multiline: true })] },
  { id: 'cards', icon: 'card', color: '#ff9f0a', fields: [F('holder'), F('number', 'hidden'), F('expiry'), F('cvv', 'pin'), F('pin', 'pin'), F('phone')] },
  { id: 'network', icon: 'network', color: '#64d2ff', fields: [F('device'), F('host'), F('port', 'number'), F('username'), F('password', 'hidden')] },
  { id: 'wifi', icon: 'wifi', color: '#32ade6', fields: [F('ssid'), F('security', 'text', { choices: ['WPA3', 'WPA2', 'WPA', 'WEP', 'none'] }), F('password', 'hidden'),
    F('hidden', 'bool')] },
  { id: 'ssh', icon: 'terminal', color: '#636366', fields: [F('host'), F('port', 'number'), F('username'), F('password', 'hidden'),
    F('private_key', 'hidden', { multiline: true }), F('fingerprint')] },
  { id: 'database', icon: 'database', color: '#bf5af2', fields: [F('engine', 'text', { choices: ['PostgreSQL', 'MySQL', 'MariaDB', 'SQLite', 'MongoDB', 'Redis', 'SQL Server', 'Oracle'] }),
    F('host'), F('port', 'number'), F('database'), F('username'), F('password', 'hidden')] },
  { id: 'apps', icon: 'apps', color: '#ff375f', fields: [F('app'), F('account'), F('password', 'hidden'), F('totp', 'totp'), F('recovery_codes', 'hidden', { multiline: true })] },
  { id: 'licenses', icon: 'key', color: '#ffd60a', fields: [F('product'), F('version'), F('license_key', 'hidden', { multiline: true }), F('email', 'email'),
    F('purchase_date', 'date'), F('expiry', 'date')], attachments: true },
  { id: 'api', icon: 'braces', color: '#ac8e68', fields: [F('service'), F('key', 'hidden'), F('secret', 'hidden'), F('expiry', 'date'), F('scopes')] },
  { id: 'identity', icon: 'id', color: '#ff6961', fields: [F('doc_type', 'text', { choices: ['dni', 'passport', 'license', 'health'] }), F('number'),
    F('holder'), F('issued', 'date'), F('expiry', 'date'), F('issuer')], attachments: true, photos: true },
  { id: 'crypto', icon: 'coin', color: '#f5a623', fields: [F('wallet'), F('address'), F('seed', 'hidden', { multiline: true }), F('password', 'hidden')], warning: true },
  { id: 'vpn', icon: 'shield', color: '#34c759', fields: [F('server'), F('vpn_type', 'text', { choices: ['WireGuard', 'OpenVPN', 'IPsec/IKEv2', 'AnyConnect'] }),
    F('username'), F('password', 'hidden')], attachments: true, settingsLink: true },
  { id: 'notes', icon: 'note', color: '#ffcc00', fields: [], attachments: true, notesFirst: true },
];

// The session keyring's categories (never created by hand)
export const SYSTEM_CATS = [
  { id: 'keyring', icon: 'lock', color: '#8e8e93', fields: [F('service'), F('username'), F('password', 'hidden')] },
  { id: 'secrets', icon: 'key', color: '#8e8e93', fields: [F('url', 'url'), F('username'), F('password', 'hidden')] },
];

export const FIELD_TYPES = ['text', 'hidden', 'url', 'date', 'number', 'email', 'totp', 'file', 'pin', 'multiline'];

// The glyphs a user category can pick (drawn in glyphs.jsx; no color emoji).
export const ICONS = GLYPH_KINDS;
export const COLORS = ['#0a84ff', '#5e5ce6', '#bf5af2', '#ff375f', '#ff453a', '#ff9f0a', '#ffd60a', '#30d158', '#64d2ff', '#ac8e68', '#8e8e93'];

export const builtinById = (id) => BUILTIN.find((c) => c.id === id) || SYSTEM_CATS.find((c) => c.id === id);

// Visible name of a category (built-in or the user's)
export function catName(cat) {
  if (!cat) return '';
  if (cat.builtin || builtinById(cat.id) && !cat.name) return t(`passwords.cat.${cat.id}`);
  return cat.name || cat.id;
}

// A field's label: its own (added fields) or the template's
export function fieldLabel(f) {
  if (f.l) return f.l;
  const k = String(f.k || '');
  return /^c\d+$/.test(k) ? t('passwords.field.custom') : t(`passwords.field.${k}`);
}

// Every category: built-in + the user's (a tree by "parent")
export function allCategories(userCats = []) {
  const builtins = BUILTIN.map((c) => ({ ...c, builtin: true }));
  const users = (userCats || []).filter((c) => c && c.id && !builtinById(c.id)).map((c) => ({ ...c, builtin: false }));
  return [...builtins, ...users];
}

// Fields for a new item of the category (user categories inherit their template's)
export function templateFields(cat, cats) {
  if (!cat) return BUILTIN[0].fields;
  if (cat.fields && cat.fields.length) return cat.fields;
  const base = builtinById(cat.template || '') || (cat.parent && cats ? templateFields(cats.find((c) => c.id === cat.parent), cats) : null);
  return Array.isArray(base) ? base : base?.fields || BUILTIN[0].fields;
}
