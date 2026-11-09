// texts.js — the words the screens build at render time (docs/LANGUAGES.md: no lists of text as module constants):
// error messages for every lord-sql code, filter operators, field types' defaults, relative times, a favorite's line.
import { t as tr, formatDate, formatNumber } from '@lordos/sdk';
import { errorCode } from './model.js';

// An Error from sqlRequest (or the app's own checks) → one sentence
export function errText(e) {
  const code = e?.code;
  const d = e?.data || {};
  switch (code) {
    case 'sql': return tr('databases.err.sql', { message: d.message || e.message || '', errno: d.errno ?? '—', sqlstate: d.sqlstate || '—' });
    case 'readonly': return tr('databases.err.readonly');
    case 'nokey': return tr('databases.err.nokey');
    case 'view': return tr('databases.err.view');
    case 'badhex': return tr('databases.err.badhex');
    case 'norows': return tr('databases.err.norows');
    case 'save': return tr('databases.err.save', { message: e.message || '' });
    default: break;
  }
  switch (errorCode(e)) {
    case 'unavailable': return tr('databases.err.unavailable');
    case 'notfound': return tr('databases.err.notfound');
    case 'dns': return tr('databases.err.dns', { message: d.message || '' });
    case 'connect': return tr('databases.err.connect', { message: d.message || '' });
    case 'tls': return tr('databases.err.tls', { message: d.message || '' });
    case 'cert': return tr('databases.err.cert');
    case 'auth': return tr('databases.err.auth');
    case 'closed': return tr('databases.err.closed');
    case 'busy': return tr('databases.err.busy');
    case 'toolarge': return tr('databases.err.toolarge');
    case 'unsupported': return tr('databases.err.unsupported');
    default: return tr('databases.err.other', { message: d.message || e?.message || String(e || '') });
  }
}

// why a connection closed (the 'closed' event's reason, or 'crash')
export const closedText = (reason) => (reason === 'crash' ? tr('databases.closed.crash') : reason && reason !== 'closed' ? tr('databases.closed.reason', { reason }) : tr('databases.closed.plain'));

// the Content filter's operators, as [value, label]
export const opOptions = () => [
  ['=', tr('databases.op.eq')], ['!=', tr('databases.op.ne')], ['>', tr('databases.op.gt')], ['<', tr('databases.op.lt')],
  ['>=', tr('databases.op.ge')], ['<=', tr('databases.op.le')], ['contains', tr('databases.op.contains')], ['notContains', tr('databases.op.notContains')],
  ['begins', tr('databases.op.begins')], ['ends', tr('databases.op.ends')], ['isNull', tr('databases.op.isNull')], ['notNull', tr('databases.op.notNull')],
  ['in', tr('databases.op.in')], ['between', tr('databases.op.between')],
];

export const typeOptions = () => [
  ['tcp', tr('databases.type.tcp')], ['socket', tr('databases.type.socket')], ['ssh', tr('databases.type.ssh')], ['cloud', tr('databases.type.cloud')],
];
export const cloudOptions = () => [
  ['rds', tr('databases.cloud.rds')], ['azure', tr('databases.cloud.azure')], ['gcp', tr('databases.cloud.gcp')], ['other', tr('databases.cloud.other')],
];
export const defaultOptions = () => [
  ['none', tr('databases.field.defNone')], ['null', tr('databases.field.defNull')], ['value', tr('databases.field.defValue')], ['expr', tr('databases.field.defExpr')],
];
export const indexKindOptions = () => [
  ['INDEX', tr('databases.index.kindIndex')], ['UNIQUE', tr('databases.index.kindUnique')], ['PRIMARY', tr('databases.index.kindPrimary')], ['FULLTEXT', tr('databases.index.kindFulltext')],
];

// "2 hours ago", "yesterday", a date
export function ago(at, now = Date.now()) {
  if (!at) return '';
  const s = Math.max(0, (now - at) / 1000);
  if (s < 60) return tr('databases.ago.now');
  if (s < 3600) return tr('databases.ago.minutes', { n: Math.round(s / 60) });
  if (s < 86400) return tr('databases.ago.hours', { n: Math.round(s / 3600) });
  if (s < 2 * 86400) return tr('databases.ago.yesterday');
  return formatDate(new Date(at));
}

// "MariaDB 11.4 · SSH", "MySQL 8.4 · socket", "192.168.1.40"
export function favLine(f, server) {
  const where = f.type === 'socket' ? tr('databases.type.socketLower')
    : f.type === 'ssh' ? tr('databases.type.sshUpper') : f.type === 'cloud' ? tr('databases.type.cloud') : f.host;
  const parts = [server, where, f.readOnly ? tr('databases.readOnlyLower') : null].filter(Boolean);
  return parts.join(' · ');
}
export const serverName = (srv) => (srv && srv.version ? `${srv.flavor === 'mariadb' ? 'MariaDB' : 'MySQL'} ${String(srv.version).split('-')[0].split('.').slice(0, 2).join('.')}` : '');

export const n = (v) => (v === null || v === undefined || v === '' ? '—' : formatNumber(Number(v)));

// a favorite's problems (model.validateFavorite) → one line
export const favProblems = (list) => list.map((p) => tr(`databases.fav.err.${p.code}`)).filter((x, i, a) => a.indexOf(x) === i).join(' ');
export const fieldProblems = (list) => list.map((p) => tr(`databases.field.err.${p.code}`)).filter((x, i, a) => a.indexOf(x) === i).join(' ');
