// model.js — Databases without a screen (docs/DATABASES.md): MySQL identifiers and literals, the Content filter, pages,
// sort, the UPDATE / DELETE / INSERT for an edited cell or row, Structure's SHOW parsers and the ALTER TABLE it shows
// before applying, the Query editor's statements (split, the one under the cursor, safe mode, read-only), completion,
// "Copy as" and the favorites' checks. Pure: tested by server/tests/databases_model_test.mjs.

// ---- identifiers and values ----

// `name`, with a backtick inside doubled
export const quoteId = (name) => `\`${String(name ?? '').replace(/`/g, '``')}\``;
// `db`.`table` (just `table` without a database)
export const qualified = (db, table) => (db ? `${quoteId(db)}.${quoteId(table)}` : quoteId(table));

// how lord-sql sends a binary value (a column with charset 63): "\u0000hex:<hex>"
export const HEX_PREFIX = '\u0000hex:';
export const isHexValue = (v) => typeof v === 'string' && v.startsWith(HEX_PREFIX);
export const hexOf = (v) => (isHexValue(v) ? v.slice(HEX_PREFIX.length) : '');

const ESC = { '\0': '\\0', '\n': '\\n', '\r': '\\r', '\\': '\\\\', "'": "\\'", '"': '\\"', '\x1a': '\\Z' };
export const escapeString = (s) => String(s).replace(/[\0\n\r\\'"\x1a]/g, (c) => ESC[c]);

// A value as a MySQL literal: NULL, a number, X'…' for binary, or a quoted string.
export function literal(v) {
  if (v === null || v === undefined) return 'NULL';
  if (typeof v === 'boolean') return v ? '1' : '0';
  if (typeof v === 'number') return Number.isFinite(v) ? String(v) : 'NULL';
  if (typeof v === 'bigint') return String(v);
  if (isHexValue(v)) {
    const hex = hexOf(v);
    if (!/^(?:[0-9a-fA-F]{2})*$/.test(hex)) throw new Error('badhex');
    return hex ? `X'${hex.toUpperCase()}'` : "''";
  }
  return `'${escapeString(v)}'`;
}

// what a person types in a cell for a binary column: 0x… becomes the hex form
export function cellInput(text, binary) {
  if (binary && /^0x(?:[0-9a-fA-F]{2})*$/.test(text)) return HEX_PREFIX + text.slice(2).toLowerCase();
  return text;
}
// how a cell shows a value (binary as 0x…, cut at max characters)
export function cellText(v, max = 300) {
  if (v === null || v === undefined) return 'NULL';
  if (isHexValue(v)) { const h = hexOf(v); return `0x${h.length > max ? `${h.slice(0, max)}…` : h.toUpperCase()}`; }
  const s = String(v);
  const one = s.length > max ? `${s.slice(0, max)}…` : s;
  return one.replace(/\r?\n/g, ' ↵ ').replace(/\t/g, ' ');
}

// ---- Content: filter, sort, pages ----

export const FILTER_OPS = ['=', '!=', '>', '<', '>=', '<=', 'contains', 'notContains', 'begins', 'ends', 'isNull', 'notNull', 'in', 'between'];
export const opNeedsValue = (op) => op !== 'isNull' && op !== 'notNull';
export const likeEscape = (s) => String(s).replace(/[\\%_]/g, (c) => `\\${c}`);

// "a, 'b,c', 3" → ['a', 'b,c', '3'] (an empty item without quotes is dropped)
export function splitList(text) {
  const out = [];
  const s = String(text ?? '');
  let cur = '', q = null, quoted = false;
  const flush = () => { if (quoted) out.push(cur); else if (cur.trim()) out.push(cur.trim()); cur = ''; quoted = false; };
  for (let i = 0; i < s.length; i++) {
    const c = s[i];
    if (q) {
      if (c === '\\' && i + 1 < s.length) { cur += s[++i]; continue; }
      if (c === q) { if (s[i + 1] === q) { cur += c; i++; } else q = null; continue; }
      cur += c;
    } else if ((c === "'" || c === '"') && !cur.trim() && !quoted) { q = c; quoted = true; cur = ''; } else if (c === ',') flush();
    else if (!quoted) cur += c;
  }
  flush();
  return out;
}

// One rule → a condition, or null when it has nothing to filter on.
export function ruleSql(rule) {
  if (!rule || !rule.column || !FILTER_OPS.includes(rule.op)) return null;
  const c = quoteId(rule.column);
  const v = rule.value ?? '';
  switch (rule.op) {
    case 'isNull': return `${c} IS NULL`;
    case 'notNull': return `${c} IS NOT NULL`;
    case 'contains': return `${c} LIKE ${literal(`%${likeEscape(v)}%`)}`;
    case 'notContains': return `${c} NOT LIKE ${literal(`%${likeEscape(v)}%`)}`;
    case 'begins': return `${c} LIKE ${literal(`${likeEscape(v)}%`)}`;
    case 'ends': return `${c} LIKE ${literal(`%${likeEscape(v)}`)}`;
    case 'in': {
      const items = splitList(v);
      if (!items.length) return null;
      return `${c} IN (${items.map(literal).join(', ')})`;
    }
    case 'between': return `${c} BETWEEN ${literal(v)} AND ${literal(rule.value2 ?? '')}`;
    default: return `${c} ${rule.op} ${literal(v)}`;
  }
}

// rules (joined with AND) → " WHERE …" or ''
export function whereSql(rules) {
  const parts = (rules || []).filter((r) => r && r.enabled !== false).map(ruleSql).filter(Boolean);
  return parts.length ? ` WHERE ${parts.join(' AND ')}` : '';
}

export const PAGE_SIZE = 1000;
export function orderSql(sort) {
  if (!sort || !sort.column) return '';
  return ` ORDER BY ${quoteId(sort.column)} ${sort.dir === 'desc' ? 'DESC' : 'ASC'}`;
}
export function pageSql({ db, table, rules, sort, page = 0, pageSize = PAGE_SIZE }) {
  const p = Math.max(0, Math.floor(page) || 0);
  return `SELECT * FROM ${qualified(db, table)}${whereSql(rules)}${orderSql(sort)} LIMIT ${pageSize}${p ? ` OFFSET ${p * pageSize}` : ''}`;
}
export const countSql = ({ db, table, rules }) => `SELECT COUNT(*) FROM ${qualified(db, table)}${whereSql(rules)}`;
export const pageCount = (total, pageSize = PAGE_SIZE) => Math.max(1, Math.ceil((Number(total) || 0) / pageSize));
// a click on a column header: none → asc → desc → none (another column starts at asc)
export function nextSort(sort, column) {
  if (!sort || sort.column !== column) return { column, dir: 'asc' };
  if (sort.dir === 'asc') return { column, dir: 'desc' };
  return null;
}
// "Rows 1–1,000": the first and last row of a page (1-based), for a page holding n rows
export const pageRange = (page, n, pageSize = PAGE_SIZE) => (n ? [page * pageSize + 1, page * pageSize + n] : [0, 0]);

// ---- editing rows (keyed by the primary key, or a unique key whose columns can't be NULL) ----

// { cols } or { error: 'nokey' | 'view' }
export function rowKey(fields, indexes, { view = false } = {}) {
  if (view) return { error: 'view' };
  const pk = (indexes || []).find((x) => x.name === 'PRIMARY');
  if (pk) return { cols: pk.columns.map((c) => c.name) };
  const byName = new Map((fields || []).map((f) => [f.name, f]));
  for (const x of indexes || []) {
    if (!x.unique || x.columns.some((c) => c.sub)) continue;
    if (x.columns.every((c) => byName.get(c.name) && !byName.get(c.name).nullable)) return { cols: x.columns.map((c) => c.name) };
  }
  const pkFields = (fields || []).filter((f) => f.key === 'PRI');
  if (pkFields.length && !indexes?.length) return { cols: pkFields.map((f) => f.name) };
  return { error: 'nokey' };
}

function keyWhere(key, columns, row) {
  const parts = key.cols.map((k) => {
    const i = columns.indexOf(k);
    if (i < 0) throw new Error('nokey');
    const v = row[i];
    return v === null || v === undefined ? `${quoteId(k)} IS NULL` : `${quoteId(k)} = ${literal(v)}`;
  });
  return parts.join(' AND ');
}

// UPDATE for one edited cell: { sql } or { error }
export function updateCellSql({ db, table, key, columns, row, column, value }) {
  if (!key || key.error) return { error: key?.error || 'nokey' };
  try {
    return { sql: `UPDATE ${qualified(db, table)} SET ${quoteId(column)} = ${literal(value)} WHERE ${keyWhere(key, columns, row)} LIMIT 1` };
  } catch (e) { return { error: e.message === 'badhex' ? 'badhex' : 'nokey' }; }
}

// SELECT of one row again by its key (after an edit, to show what the server kept)
export function selectRowSql({ db, table, key, columns, row }) {
  if (!key || key.error) return { error: key?.error || 'nokey' };
  try { return { sql: `SELECT * FROM ${qualified(db, table)} WHERE ${keyWhere(key, columns, row)} LIMIT 1` }; } catch (e) { return { error: 'nokey' }; }
}

export function deleteRowsSql({ db, table, key, columns, rows }) {
  if (!key || key.error) return { error: key?.error || 'nokey' };
  if (!rows?.length) return { error: 'norows' };
  try {
    if (key.cols.length === 1) {
      const i = columns.indexOf(key.cols[0]);
      if (i < 0) return { error: 'nokey' };
      if (rows.every((r) => r[i] !== null && r[i] !== undefined)) {
        return { sql: `DELETE FROM ${qualified(db, table)} WHERE ${quoteId(key.cols[0])} ${rows.length === 1 ? `= ${literal(rows[0][i])}` : `IN (${rows.map((r) => literal(r[i])).join(', ')})`} LIMIT ${rows.length}` };
      }
    }
    return { sql: `DELETE FROM ${qualified(db, table)} WHERE ${rows.map((r) => `(${keyWhere(key, columns, r)})`).join(' OR ')} LIMIT ${rows.length}` };
  } catch (e) { return { error: 'nokey' }; }
}

// values: { column: value } — a column left out takes its default
export function insertRowSql({ db, table, values }) {
  const cols = Object.keys(values || {}).filter((k) => values[k] !== undefined);
  if (!cols.length) return { sql: `INSERT INTO ${qualified(db, table)} () VALUES ()` };
  try {
    return { sql: `INSERT INTO ${qualified(db, table)} (${cols.map(quoteId).join(', ')}) VALUES (${cols.map((k) => literal(values[k])).join(', ')})` };
  } catch (e) { return { error: 'badhex' }; }
}

// Duplicate a row: every column but the auto-increment ones (and generated ones, which can't be written)
export function duplicateValues(fields, columns, row) {
  const out = {};
  const byName = new Map((fields || []).map((f) => [f.name, f]));
  columns.forEach((c, i) => {
    const f = byName.get(c);
    if (f && (f.autoIncrement || /\b(?:VIRTUAL|STORED|PERSISTENT) GENERATED\b/i.test(f.extra || ''))) return;
    out[c] = row[i];
  });
  return out;
}

// ---- Structure: what SHOW … answers, as models ----

// a Result's rows as objects by column name
export function rowsOf(result) {
  const names = (result?.columns || []).map((c) => c.name);
  return (result?.rows || []).map((r) => Object.fromEntries(names.map((n, i) => [n, r[i]])));
}
const pick = (o, ...keys) => { for (const k of keys) if (o[k] !== undefined) return o[k]; const lower = Object.keys(o).find((k) => keys.some((x) => x.toLowerCase() === k.toLowerCase())); return lower ? o[lower] : undefined; };

// "int(11) unsigned zerofill" → { type: 'INT', length: '11', unsigned, zerofill }
export function parseType(raw) {
  const s = String(raw || '').trim();
  const m = /^([a-zA-Z ]+?)\s*(?:\(((?:'(?:[^']|'')*'|[^)'])*)\))?((?:\s+\w+)*)\s*$/.exec(s);
  if (!m) return { type: s.toUpperCase(), length: '', unsigned: false, zerofill: false, binary: false };
  const words = (m[3] || '').trim().toLowerCase().split(/\s+/).filter(Boolean);
  return {
    type: m[1].trim().toUpperCase(),
    length: m[2] !== undefined ? m[2] : '',
    unsigned: words.includes('unsigned'),
    zerofill: words.includes('zerofill'),
    binary: words.includes('binary'),
  };
}

const EXPR_DEFAULT = /^(?:current_timestamp|now|localtime|localtimestamp|curdate|curtime|current_date|current_time|utc_timestamp|uuid|sysdate)(?:\(\s*\d*\s*\))?$/i;
// SHOW FULL COLUMNS's Default: { kind: 'none' | 'null' | 'value' | 'expr', value }. MariaDB 10.2.7+ quotes string
// defaults ('pending') and writes NULL as the word; MySQL gives the bare value and SQL NULL.
export function parseDefault(raw, nullable) {
  if (raw === null || raw === undefined) return nullable ? { kind: 'null', value: '' } : { kind: 'none', value: '' };
  const s = String(raw);
  if (s === 'NULL' && nullable) return { kind: 'null', value: '' };
  if (EXPR_DEFAULT.test(s.trim())) return { kind: 'expr', value: s.trim() };
  const q = /^'((?:[^']|'')*)'$/s.exec(s);
  if (q) return { kind: 'value', value: q[1].replace(/''/g, "'").replace(/\\\\/g, '\\') };
  return { kind: 'value', value: s };
}

export function parseColumns(result) {
  return rowsOf(result).map((r) => {
    const t = parseType(pick(r, 'Type'));
    const nullable = String(pick(r, 'Null') || '').toUpperCase() === 'YES';
    const extra = String(pick(r, 'Extra') || '');
    return {
      name: pick(r, 'Field') ?? '',
      ...t,
      rawType: pick(r, 'Type') ?? '',
      collation: pick(r, 'Collation') ?? '',
      nullable,
      key: pick(r, 'Key') || '',
      def: parseDefault(pick(r, 'Default'), nullable),
      extra,
      autoIncrement: /auto_increment/i.test(extra),
      onUpdate: (/on update (\S+(?:\(\d*\))?)/i.exec(extra) || [])[1] || '',
      comment: pick(r, 'Comment') || '',
    };
  });
}

export function parseIndexes(result) {
  const map = new Map();
  for (const r of rowsOf(result)) {
    const name = pick(r, 'Key_name');
    if (!map.has(name)) {
      map.set(name, { name, unique: String(pick(r, 'Non_unique')) === '0', type: pick(r, 'Index_type') || 'BTREE', cardinality: null,
        comment: pick(r, 'Index_comment') || pick(r, 'Comment') || '', columns: [] });
    }
    const x = map.get(name);
    x.columns.push({ seq: Number(pick(r, 'Seq_in_index')) || x.columns.length + 1, name: pick(r, 'Column_name') ?? '', sub: pick(r, 'Sub_part') ? Number(pick(r, 'Sub_part')) : null });
    const card = pick(r, 'Cardinality');
    if (card !== null && card !== undefined && card !== '') x.cardinality = Math.max(x.cardinality ?? 0, Number(card));
  }
  for (const x of map.values()) x.columns.sort((a, b) => a.seq - b.seq);
  return [...map.values()].sort((a, b) => (a.name === 'PRIMARY' ? -1 : b.name === 'PRIMARY' ? 1 : 0));
}

const num = (v) => (v === null || v === undefined || v === '' ? null : Number(v));
export function parseStatus(result) {
  const r = rowsOf(result)[0];
  if (!r) return null;
  const coll = pick(r, 'Collation') || '';
  return {
    name: pick(r, 'Name') ?? '',
    engine: pick(r, 'Engine') || '',
    rowFormat: pick(r, 'Row_format') || '',
    rows: num(pick(r, 'Rows')),
    avgRow: num(pick(r, 'Avg_row_length')),
    data: num(pick(r, 'Data_length')),
    index: num(pick(r, 'Index_length')),
    free: num(pick(r, 'Data_free')),
    autoIncrement: num(pick(r, 'Auto_increment')),
    created: pick(r, 'Create_time') || '',
    updated: pick(r, 'Update_time') || '',
    collation: coll,
    charset: coll ? coll.split('_')[0] : '',
    comment: pick(r, 'Comment') || '',
    view: String(pick(r, 'Comment') || '').toUpperCase() === 'VIEW' && !pick(r, 'Engine'),
  };
}

// SHOW CREATE TABLE / VIEW → the statement
export function parseCreate(result) {
  const r = rowsOf(result)[0];
  if (!r) return '';
  return pick(r, 'Create Table', 'Create View', 'Create Procedure', 'Create Function') || '';
}

// SHOW FULL TABLES → { tables, views }
export function parseTables(result) {
  const tables = [], views = [];
  for (const r of result?.rows || []) {
    const name = r[0];
    if (name === null || name === undefined) continue;
    (/VIEW/i.test(String(r[1] || '')) ? views : tables).push(name);
  }
  const by = (a, b) => a.localeCompare(b, undefined, { sensitivity: 'base' });
  return { tables: tables.sort(by), views: views.sort(by) };
}

export const tablesSql = (db) => `SHOW FULL TABLES FROM ${quoteId(db)}`;
export const routinesSql = (db) => `SELECT ROUTINE_NAME, ROUTINE_TYPE FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = ${literal(db)} ORDER BY ROUTINE_NAME`;
export const columnsSql = (db, table) => `SHOW FULL COLUMNS FROM ${qualified(db, table)}`;
export const indexesSql = (db, table) => `SHOW INDEX FROM ${qualified(db, table)}`;
export const statusSql = (db, table) => `SHOW TABLE STATUS FROM ${quoteId(db)} LIKE ${literal(likeEscape(table))}`;
export const createSql = (db, table, view) => `SHOW CREATE ${view ? 'VIEW' : 'TABLE'} ${qualified(db, table)}`;
export const allColumnsSql = (db) => `SELECT TABLE_NAME, COLUMN_NAME, COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = ${literal(db)} ORDER BY TABLE_NAME, ORDINAL_POSITION`;

// ---- Structure: field and index edits as ALTER TABLE ----

export const FIELD_TYPES = ['TINYINT', 'SMALLINT', 'MEDIUMINT', 'INT', 'BIGINT', 'DECIMAL', 'FLOAT', 'DOUBLE', 'BIT', 'BOOLEAN',
  'CHAR', 'VARCHAR', 'BINARY', 'VARBINARY', 'TINYTEXT', 'TEXT', 'MEDIUMTEXT', 'LONGTEXT', 'TINYBLOB', 'BLOB', 'MEDIUMBLOB', 'LONGBLOB',
  'ENUM', 'SET', 'DATE', 'DATETIME', 'TIMESTAMP', 'TIME', 'YEAR', 'JSON', 'UUID', 'INET4', 'INET6',
  'GEOMETRY', 'POINT', 'LINESTRING', 'POLYGON', 'MULTIPOINT', 'MULTILINESTRING', 'MULTIPOLYGON', 'GEOMETRYCOLLECTION'];
const INTEGER = new Set(['TINYINT', 'SMALLINT', 'MEDIUMINT', 'INT', 'INTEGER', 'BIGINT']);
const NUMERIC = new Set([...INTEGER, 'DECIMAL', 'NUMERIC', 'FLOAT', 'DOUBLE', 'REAL', 'BIT', 'BOOLEAN', 'BOOL', 'YEAR']);
const TEXTUAL = new Set(['CHAR', 'VARCHAR', 'TINYTEXT', 'TEXT', 'MEDIUMTEXT', 'LONGTEXT', 'ENUM', 'SET']);
const NO_DEFAULT = new Set(['TINYTEXT', 'TEXT', 'MEDIUMTEXT', 'LONGTEXT', 'TINYBLOB', 'BLOB', 'MEDIUMBLOB', 'LONGBLOB', 'JSON', 'GEOMETRY']);
export const isNumericType = (type) => NUMERIC.has(String(type).toUpperCase());
export const isTextType = (type) => TEXTUAL.has(String(type).toUpperCase());
// which types need or take a length: 'required' | 'optional' | 'none'
export function lengthRule(type) {
  const t = String(type).toUpperCase();
  if (['VARCHAR', 'VARBINARY', 'ENUM', 'SET'].includes(t)) return 'required';
  if ([...INTEGER, 'DECIMAL', 'FLOAT', 'DOUBLE', 'BIT', 'CHAR', 'BINARY', 'DATETIME', 'TIMESTAMP', 'TIME', 'YEAR', 'TEXT', 'BLOB'].includes(t)) return 'optional';
  return 'none';
}

const isName = (s) => typeof s === 'string' && s.length > 0 && s.length <= 64 && !/[\0]/.test(s) && s.trim() === s;

// problems with a field as edited: [{ field, code }]
export function validateField(f) {
  const out = [];
  if (!isName(f.name)) out.push({ field: 'name', code: 'name' });
  const type = String(f.type || '').toUpperCase();
  if (!FIELD_TYPES.includes(type) && !['INTEGER', 'NUMERIC', 'REAL', 'BOOL'].includes(type)) out.push({ field: 'type', code: 'type' });
  const rule = lengthRule(type);
  const len = String(f.length ?? '').trim();
  if (rule === 'required' && !len) out.push({ field: 'length', code: 'lengthNeeded' });
  if (len && rule === 'none') out.push({ field: 'length', code: 'lengthNone' });
  if (len && (type === 'ENUM' || type === 'SET')) {
    if (!/^'(?:[^'\\]|''|\\.)*'(?:\s*,\s*'(?:[^'\\]|''|\\.)*')*$/.test(len)) out.push({ field: 'length', code: 'enumValues' });
  } else if (len && (type === 'DECIMAL' || type === 'FLOAT' || type === 'DOUBLE')) {
    if (!/^\d{1,2}(?:\s*,\s*\d{1,2})?$/.test(len)) out.push({ field: 'length', code: 'length' });
  } else if (len && !/^\d{1,5}$/.test(len)) out.push({ field: 'length', code: 'length' });
  if (f.def?.kind === 'null' && !f.nullable) out.push({ field: 'default', code: 'nullDefault' });
  if (f.def?.kind === 'value' && NO_DEFAULT.has(type)) out.push({ field: 'default', code: 'noDefault' });
  if (f.autoIncrement && !INTEGER.has(type) && !['FLOAT', 'DOUBLE'].includes(type)) out.push({ field: 'extra', code: 'autoIncrement' });
  if (f.collation && !/^[a-z0-9_]{1,64}$/i.test(f.collation)) out.push({ field: 'collation', code: 'collation' });
  return out;
}

export function defaultSql(f) {
  const d = f.def || { kind: 'none' };
  if (d.kind === 'null') return ' DEFAULT NULL';
  if (d.kind === 'expr') return ` DEFAULT ${d.value.toUpperCase()}`;
  if (d.kind === 'value') {
    if (isNumericType(f.type) && /^-?\d+(?:\.\d+)?(?:e[+-]?\d+)?$/i.test(d.value)) return ` DEFAULT ${d.value}`;
    if (String(f.type).toUpperCase() === 'BIT' && /^b'[01]*'$/i.test(d.value)) return ` DEFAULT ${d.value}`;
    return ` DEFAULT ${literal(d.value)}`;
  }
  return '';
}

// The column definition after its name: INT(11) UNSIGNED NOT NULL DEFAULT 0 AUTO_INCREMENT COMMENT '…'
export function columnDef(f) {
  const type = String(f.type).toUpperCase();
  let s = `${quoteId(f.name)} ${type}`;
  const len = String(f.length ?? '').trim();
  if (len) s += `(${len})`;
  if (f.unsigned && isNumericType(type)) s += ' UNSIGNED';
  if (f.zerofill && isNumericType(type)) s += ' ZEROFILL';
  if (f.binary && isTextType(type)) s += ' BINARY';
  if (f.collation && isTextType(type)) s += ` COLLATE ${f.collation}`;
  s += f.nullable ? ' NULL' : ' NOT NULL';
  s += defaultSql(f);
  if (f.autoIncrement) s += ' AUTO_INCREMENT';
  if (f.onUpdate) s += ` ON UPDATE ${String(f.onUpdate).toUpperCase()}`;
  if (f.comment) s += ` COMMENT ${literal(f.comment)}`;
  return s;
}

const position = (after) => (after === undefined ? '' : after === null ? ' FIRST' : ` AFTER ${quoteId(after)}`);

// One change → its clause. change: { op: 'add', field, after } | { op: 'change', from, field, after } | { op: 'drop', name }
//   | { op: 'addIndex', index: { name, kind, columns: [{ name, sub }] } } | { op: 'dropIndex', name }
export function alterClause(c) {
  switch (c.op) {
    case 'add': return `ADD ${columnDef(c.field)}${position(c.after)}`;
    case 'change': return c.from === c.field.name ? `MODIFY ${columnDef(c.field)}${position(c.after)}` : `CHANGE ${quoteId(c.from)} ${columnDef(c.field)}${position(c.after)}`;
    case 'drop': return `DROP ${quoteId(c.name)}`;
    case 'addIndex': {
      const x = c.index;
      const cols = `(${x.columns.map((k) => `${quoteId(k.name)}${k.sub ? `(${Number(k.sub)})` : ''}`).join(', ')})`;
      if (x.kind === 'PRIMARY') return `ADD PRIMARY KEY ${cols}`;
      const kind = x.kind === 'UNIQUE' ? 'UNIQUE INDEX' : x.kind === 'FULLTEXT' ? 'FULLTEXT INDEX' : x.kind === 'SPATIAL' ? 'SPATIAL INDEX' : 'INDEX';
      return `ADD ${kind}${x.name ? ` ${quoteId(x.name)}` : ''} ${cols}`;
    }
    case 'dropIndex': return c.name === 'PRIMARY' ? 'DROP PRIMARY KEY' : `DROP INDEX ${quoteId(c.name)}`;
    default: throw new Error('op');
  }
}
export function alterSql(db, table, changes) {
  if (!changes?.length) return '';
  return `ALTER TABLE ${qualified(db, table)}\n  ${changes.map(alterClause).join(',\n  ')}`;
}

export function validateIndex(x, fields) {
  const out = [];
  if (x.kind !== 'PRIMARY' && x.name && !isName(x.name)) out.push({ field: 'name', code: 'name' });
  if (x.name === 'PRIMARY' && x.kind !== 'PRIMARY') out.push({ field: 'name', code: 'name' });
  if (!x.columns?.length) out.push({ field: 'columns', code: 'columns' });
  const names = new Set((fields || []).map((f) => f.name));
  for (const c of x.columns || []) if (!names.has(c.name)) out.push({ field: 'columns', code: 'columns' });
  return out;
}

// a new name that isn't taken: "field", "field_2", …
export function freeName(base, taken) {
  const set = new Set(taken);
  if (!set.has(base)) return base;
  for (let i = 2; ; i++) if (!set.has(`${base}_${i}`)) return `${base}_${i}`;
}

// ---- Query: statements ----

// The statements of a buffer: [{ sql, start, end }] (offsets into text; end is where the statement's text stops, before
// its delimiter). Quotes ('…', "…" with \ and doubling), `identifiers`, -- and # comments, /* … */ and DELIMITER lines.
export function splitStatements(text) {
  const s = String(text ?? '');
  const out = [];
  let delim = ';';
  let start = 0;
  let i = 0;
  let lineStart = true;
  const push = (end, next) => {
    const raw = s.slice(start, end);
    const lead = raw.length - raw.trimStart().length;
    const sql = raw.trim();
    if (sql && stripComments(sql).trim()) out.push({ sql, start: start + lead, end: start + lead + sql.length });
    start = next;
  };
  while (i < s.length) {
    const c = s[i];
    // DELIMITER at the start of a statement's first line
    if (lineStart && /^[ \t]*delimiter[ \t]+\S/i.test(s.slice(i, i + 64)) && !s.slice(start, i).trim()) {
      const eol = s.indexOf('\n', i);
      const line = s.slice(i, eol < 0 ? s.length : eol);
      delim = line.trim().split(/\s+/)[1];
      i = eol < 0 ? s.length : eol + 1;
      start = i;
      lineStart = true;
      continue;
    }
    lineStart = false;
    if (c === "'" || c === '"' || c === '`') {
      i++;
      while (i < s.length) {
        if (s[i] === '\\' && c !== '`') { i += 2; continue; }
        if (s[i] === c) { if (s[i + 1] === c) { i += 2; continue; } break; }
        i++;
      }
      i++;
      continue;
    }
    if (c === '#' || (c === '-' && s[i + 1] === '-' && (i + 2 >= s.length || /\s/.test(s[i + 2])))) {
      const eol = s.indexOf('\n', i);
      i = eol < 0 ? s.length : eol;
      continue;
    }
    if (c === '/' && s[i + 1] === '*') {
      const e = s.indexOf('*/', i + 2);
      i = e < 0 ? s.length : e + 2;
      continue;
    }
    if (s.startsWith(delim, i)) {
      push(i, i + delim.length);
      i += delim.length;
      continue;
    }
    if (c === '\n') lineStart = true;
    i++;
  }
  push(s.length, s.length);
  return out;
}

// the statement under the cursor: the one that contains it, else the last one before it, else the first after it
export function statementAt(text, offset) {
  const list = splitStatements(text);
  if (!list.length) return null;
  const s = String(text ?? '');
  for (const st of list) {
    let end = st.end;
    while (end < s.length && /[ \t]/.test(s[end])) end++;      // the delimiter and spaces after it, on its line
    if (offset >= st.start && offset <= end + 2 && (offset <= st.end || !/\n/.test(s.slice(st.end, offset)))) return st;
  }
  let before = null;
  for (const st of list) if (st.start <= offset) before = st;
  return before || list[0];
}

// a position { l, c } in lines → an offset in the text joined with \n
export const offsetOf = (lines, p) => lines.slice(0, p.l).reduce((n, x) => n + x.length + 1, 0) + p.c;
// and back
export function posOf(text, offset) {
  const before = String(text).slice(0, offset).split('\n');
  return { l: before.length - 1, c: before[before.length - 1].length };
}

// comments out, strings and identifiers kept
export function stripComments(sql) {
  let out = '';
  const s = String(sql);
  for (let i = 0; i < s.length;) {
    const c = s[i];
    if (c === "'" || c === '"' || c === '`') {
      let j = i + 1;
      while (j < s.length) { if (s[j] === '\\' && c !== '`') { j += 2; continue; } if (s[j] === c) { if (s[j + 1] === c) { j += 2; continue; } break; } j++; }
      out += s.slice(i, j + 1); i = j + 1; continue;
    }
    if (c === '#' || (c === '-' && s[i + 1] === '-' && (i + 2 >= s.length || /\s/.test(s[i + 2])))) { const e = s.indexOf('\n', i); i = e < 0 ? s.length : e; out += ' '; continue; }
    if (c === '/' && s[i + 1] === '*') {
      const e = s.indexOf('*/', i + 2);
      // /*! … */ is MySQL's executable comment: its text runs
      if (s[i + 2] === '!') { out += ` ${s.slice(i + 3, e < 0 ? s.length : e).replace(/^\d{5,6}/, '')} `; } else out += ' ';
      i = e < 0 ? s.length : e + 2; continue;
    }
    out += c; i++;
  }
  return out;
}
// …and strings and quoted identifiers blanked, for looking at keywords
export function bareSql(sql) {
  return stripComments(sql).replace(/'(?:[^'\\]|\\.|'')*'|"(?:[^"\\]|\\.|"")*"|`(?:[^`]|``)*`/g, ' ? ');
}
export const firstWord = (sql) => ((/^[\s(]*([a-zA-Z]+)/.exec(bareSql(sql)) || [])[1] || '').toUpperCase();

// Safe mode: an UPDATE or DELETE without WHERE changes every row
export function isDangerous(sql) {
  const b = bareSql(sql);
  const w = firstWord(sql);
  const body = w === 'WITH' ? b.replace(/^[\s\S]*?\)\s*(?=(?:UPDATE|DELETE)\b)/i, '') : b;
  const kind = ((/^\s*([a-zA-Z]+)/.exec(body) || [])[1] || '').toUpperCase();
  if (kind !== 'UPDATE' && kind !== 'DELETE') return false;
  return !/\bWHERE\b/i.test(body);
}

// What a read-only (or locked production) connection still runs
const READ = new Set(['SELECT', 'SHOW', 'DESCRIBE', 'DESC', 'EXPLAIN', 'USE', 'HELP', 'TABLE', 'VALUES', 'ANALYZE']);
export function isReadOnlySql(sql) {
  const b = bareSql(sql);
  const w = firstWord(sql);
  if (/\bINTO\s+(?:OUTFILE|DUMPFILE)\b/i.test(b)) return false;
  if (/\bFOR\s+UPDATE\b|\bLOCK\s+IN\s+SHARE\s+MODE\b/i.test(b)) return false;
  if (w === 'WITH') return !/\b(?:UPDATE|DELETE|INSERT|REPLACE)\b/i.test(b);
  if (w === 'ANALYZE') return !/^\s*ANALYZE\s+(?:NO_WRITE_TO_BINLOG\s+|LOCAL\s+)?TABLE\b/i.test(b);
  if (w === 'EXPLAIN' || w === 'DESCRIBE' || w === 'DESC') return !/\bANALYZE\b/i.test(b) || isReadOnlySql(b.replace(/^\s*\w+\s+(?:ANALYZE\s+)?(?:FORMAT\s*=\s*\w+\s+)?/i, ''));
  return READ.has(w);
}

// The console never shows a password: IDENTIFIED BY '…', PASSWORD('…'), PASSWORD = '…', … AS '…' for a plugin
export function redactSql(sql) {
  return String(sql)
    .replace(/(\bIDENTIFIED\s+(?:WITH\s+\S+\s+)?(?:BY|AS)\s+(?:PASSWORD\s+)?)('(?:[^'\\]|\\.|'')*'|"(?:[^"\\]|\\.|"")*")/gi, "$1'••••'")
    .replace(/(\b(?:OLD_)?PASSWORD\s*\(\s*)('(?:[^'\\]|\\.|'')*'|"(?:[^"\\]|\\.|"")*")/gi, "$1'••••'")
    .replace(/(\bPASSWORD\s*=\s*)('(?:[^'\\]|\\.|'')*'|"(?:[^"\\]|\\.|"")*")/gi, "$1'••••'")
    .replace(/(\bMASTER_PASSWORD\s*=\s*|\bSOURCE_PASSWORD\s*=\s*)('(?:[^'\\]|\\.|'')*'|"(?:[^"\\]|\\.|"")*")/gi, "$1'••••'");
}

// ---- Query: completion ----

export const KEYWORDS = ['SELECT', 'FROM', 'WHERE', 'AND', 'OR', 'NOT', 'INSERT', 'INTO', 'VALUES', 'UPDATE', 'SET', 'DELETE', 'CREATE', 'TABLE',
  'DROP', 'ALTER', 'ADD', 'COLUMN', 'INDEX', 'PRIMARY', 'KEY', 'FOREIGN', 'REFERENCES', 'JOIN', 'LEFT', 'RIGHT', 'INNER', 'OUTER', 'CROSS',
  'ON', 'USING', 'GROUP', 'BY', 'ORDER', 'HAVING', 'LIMIT', 'OFFSET', 'AS', 'DISTINCT', 'UNION', 'ALL', 'EXISTS', 'IN', 'IS', 'NULL', 'LIKE',
  'BETWEEN', 'CASE', 'WHEN', 'THEN', 'ELSE', 'END', 'ASC', 'DESC', 'SHOW', 'DATABASES', 'TABLES', 'COLUMNS', 'DESCRIBE', 'EXPLAIN', 'USE',
  'TRUNCATE', 'REPLACE', 'DUPLICATE', 'VIEW', 'TRIGGER', 'PROCEDURE', 'FUNCTION', 'BEGIN', 'COMMIT', 'ROLLBACK', 'TRANSACTION', 'WITH',
  'RECURSIVE', 'OVER', 'PARTITION', 'WINDOW', 'INTERVAL', 'DEFAULT', 'UNIQUE', 'CONSTRAINT', 'ENGINE', 'CHARSET', 'COLLATE', 'IF', 'TRUE', 'FALSE',
  'COUNT', 'SUM', 'AVG', 'MIN', 'MAX', 'CONCAT', 'COALESCE', 'IFNULL', 'NOW', 'CURRENT_DATE', 'CURRENT_TIMESTAMP', 'DATE_FORMAT', 'DATE_ADD',
  'DATE_SUB', 'LOWER', 'UPPER', 'LENGTH', 'SUBSTRING', 'TRIM', 'ROUND', 'CAST', 'CONVERT', 'JSON_EXTRACT', 'GROUP_CONCAT'];
const FUNCS = new Set(['COUNT', 'SUM', 'AVG', 'MIN', 'MAX', 'CONCAT', 'COALESCE', 'IFNULL', 'NOW', 'CURRENT_DATE', 'CURRENT_TIMESTAMP', 'DATE_FORMAT',
  'DATE_ADD', 'DATE_SUB', 'LOWER', 'UPPER', 'LENGTH', 'SUBSTRING', 'TRIM', 'ROUND', 'CAST', 'CONVERT', 'JSON_EXTRACT', 'GROUP_CONCAT']);

// the word being typed before column c of a line: { word, from } (a `table.` qualifier is kept apart)
export function wordBefore(line, c) {
  const s = String(line).slice(0, c);
  const m = /([A-Za-z_$][\w$]*)?$/.exec(s);
  const word = m[1] || '';
  const from = c - word.length;
  const q = /([A-Za-z_$][\w$]*|`[^`]+`)\.$/.exec(s.slice(0, from));
  return { word, from, qualifier: q ? q[1].replace(/`/g, '') : '' };
}

// schema: { tables: [name], columns: { table: [{ name, type }] } }; text: the buffer (for aliases "orders o")
// → [{ label, kind: 'column' | 'table' | 'keyword' | 'function', detail }]
export function completions(prefix, schema, { qualifier = '', text = '', limit = 12 } = {}) {
  const p = String(prefix || '').toLowerCase();
  const out = [];
  const seen = new Set();
  const add = (label, kind, detail = '') => {
    const k = `${kind}:${label}`;
    if (seen.has(k) || !label.toLowerCase().startsWith(p) || label.toLowerCase() === p) return;
    seen.add(k); out.push({ label, kind, detail });
  };
  const tables = schema?.tables || [];
  const cols = schema?.columns || {};
  if (qualifier) {
    // an alias: FROM orders o / JOIN customers AS c
    const alias = new RegExp(`\\b(?:FROM|JOIN|UPDATE|INTO)\\s+\`?([\\w$]+)\`?(?:\\s+AS)?\\s+\`?${qualifier.replace(/[$]/g, '\\$')}\`?\\b`, 'i').exec(text);
    const table = cols[qualifier] ? qualifier : alias ? alias[1] : '';
    for (const c of cols[table] || []) add(c.name, 'column', `${c.type} · ${table}`);
    return out.slice(0, limit);
  }
  if (!p) return [];
  const used = tables.filter((tb) => new RegExp(`\\b${tb.replace(/[$]/g, '\\$')}\\b`, 'i').test(text));
  for (const tb of used) for (const c of cols[tb] || []) add(c.name, 'column', `${c.type} · ${tb}`);
  for (const tb of tables) add(tb, 'table');
  for (const k of KEYWORDS) add(k, FUNCS.has(k) ? 'function' : 'keyword');
  for (const tb of tables) if (!used.includes(tb)) for (const c of cols[tb] || []) add(c.name, 'column', `${c.type} · ${tb}`);
  return out.slice(0, limit);
}

// information_schema.COLUMNS rows → { table: [{ name, type }] }
export function columnsByTable(result) {
  const out = {};
  for (const r of result?.rows || []) (out[r[0]] ||= []).push({ name: r[1], type: String(r[2] || '').toUpperCase() });
  return out;
}

// ---- Copy as ----

const csvCell = (v) => {
  if (v === null || v === undefined) return 'NULL';
  const s = isHexValue(v) ? `0x${hexOf(v)}` : String(v);
  return /[",\r\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
};
export const toCsv = (columns, rows) => [columns.map((c) => csvCell(c)).join(','), ...rows.map((r) => r.map(csvCell).join(','))].join('\n');
export const toJson = (columns, rows) => JSON.stringify(rows.map((r) => Object.fromEntries(columns.map((c, i) => [c, isHexValue(r[i]) ? `0x${hexOf(r[i])}` : r[i]]))), null, 2);
export function toInserts(table, columns, rows) {
  if (!rows.length) return '';
  return `INSERT INTO ${quoteId(table || 'result')} (${columns.map(quoteId).join(', ')}) VALUES\n${rows.map((r) => `  (${r.map((v) => literal(v)).join(', ')})`).join(',\n')};`;
}

// ---- favorites ----

// red, orange, yellow, green, blue, purple, gray (favorites.json "color" 0–6; Sequel Ace's colorIndex uses the same order)
export const FAV_COLORS = ['#ff453a', '#ff9f0a', '#ffd60a', '#30d158', '#0a84ff', '#bf5af2', '#8e8e93'];
export const colorOf = (fav) => (Number.isInteger(fav?.color) && fav.color >= 0 && fav.color < FAV_COLORS.length ? FAV_COLORS[fav.color] : null);
export const FAV_TYPES = ['tcp', 'socket', 'ssh', 'cloud'];
export const TLS_MODES = ['off', 'preferred', 'required', 'verify'];
export const CLOUD_SERVICES = ['rds', 'azure', 'gcp', 'other'];
export const ID_RE = /^[a-z0-9][a-z0-9._-]{0,63}$/;

// an id from a name, not in taken: "Prod Shop" → "prod-shop", "prod-shop-2", …
export function newFavoriteId(name, taken) {
  const set = new Set(taken || []);
  let base = String(name || '').normalize('NFKD').replace(/[\u0300-\u036f]/g, '').toLowerCase().replace(/[^a-z0-9._-]+/g, '-').replace(/^[-._]+|[-._]+$/g, '').slice(0, 48);
  if (!base) base = 'connection';
  if (!set.has(base)) return base;
  for (let i = 2; ; i++) if (!set.has(`${base}-${i}`)) return `${base}-${i}`;
}
export function newGroupId(taken) {
  const set = new Set(taken || []);
  for (let i = 1; ; i++) if (!set.has(`g${i}`)) return `g${i}`;
}

export function blankFavorite(over = {}) {
  return {
    id: '', name: '', group: null, color: null, type: 'tcp', host: '', port: 3306, user: '', database: '', socket: '',
    ssh: { host: '', port: 22, user: '', key: '' },
    tls: { mode: 'preferred', ca: '', cert: '', key: '' },
    cloud: { service: '', region: '', iamProfile: '' },
    readOnly: false, production: false, timeZone: '', trust: '', quick: false,
    ...over,
  };
}
// a favorite from the file, with every key present
export function normalizeFavorite(f) {
  const b = blankFavorite();
  const o = f && typeof f === 'object' ? f : {};
  return { ...b, ...o, ssh: { ...b.ssh, ...(o.ssh || {}) }, tls: { ...b.tls, ...(o.tls || {}) }, cloud: { ...b.cloud, ...(o.cloud || {}) } };
}

const HOST_RE = /^(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*\.?$/;
const IPV6_RE = /^\[?[0-9a-fA-F:.]+(?:%[\w.-]+)?\]?$/;
export const validHost = (h) => typeof h === 'string' && h.length > 0 && h.length <= 253 && (HOST_RE.test(h) || (h.includes(':') && IPV6_RE.test(h)));
const validPort = (p) => Number.isInteger(Number(p)) && Number(p) >= 1 && Number(p) <= 65535 && String(p).trim() !== '';
const validPath = (p) => typeof p === 'string' && p.length > 0 && p.length <= 4096 && !/[\0\n]/.test(p);

// [{ field, code }]: what keeps a favorite from being saved or opened
export function validateFavorite(f, others = []) {
  const out = [];
  if (!f || typeof f !== 'object') return [{ field: 'name', code: 'name' }];
  if (!String(f.name || '').trim() || String(f.name).length > 120) out.push({ field: 'name', code: 'name' });
  if (f.id && !ID_RE.test(f.id)) out.push({ field: 'id', code: 'id' });
  if (f.id && others.some((o) => o.id === f.id && o !== f)) out.push({ field: 'id', code: 'idTaken' });
  if (!FAV_TYPES.includes(f.type)) out.push({ field: 'type', code: 'type' });
  if (f.type === 'socket') {
    if (!validPath(f.socket) || !f.socket.startsWith('/')) out.push({ field: 'socket', code: 'socket' });
  } else {
    if (!validHost(String(f.host || ''))) out.push({ field: 'host', code: 'host' });
    if (!validPort(f.port)) out.push({ field: 'port', code: 'port' });
  }
  if (String(f.user || '').length > 128 || /[\0\n]/.test(String(f.user || ''))) out.push({ field: 'user', code: 'user' });
  if (String(f.database || '').length > 64) out.push({ field: 'database', code: 'database' });
  if (f.type === 'ssh') {
    if (!validHost(String(f.ssh?.host || ''))) out.push({ field: 'sshHost', code: 'host' });
    if (!validPort(f.ssh?.port ?? 22)) out.push({ field: 'sshPort', code: 'port' });
    if (f.ssh?.key && !validPath(f.ssh.key)) out.push({ field: 'sshKey', code: 'path' });
  }
  if (!TLS_MODES.includes(f.tls?.mode || 'preferred')) out.push({ field: 'tls', code: 'tls' });
  for (const k of ['ca', 'cert', 'key']) if (f.tls?.[k] && !validPath(f.tls[k])) out.push({ field: `tls.${k}`, code: 'path' });
  if (!!f.tls?.cert !== !!f.tls?.key) out.push({ field: 'tls.cert', code: 'certPair' });
  if (f.color !== null && f.color !== undefined && !(Number.isInteger(f.color) && f.color >= 0 && f.color < FAV_COLORS.length)) out.push({ field: 'color', code: 'color' });
  if (f.type === 'cloud' && f.cloud?.service && !CLOUD_SERVICES.includes(f.cloud.service)) out.push({ field: 'cloud', code: 'cloud' });
  return out;
}

// A favorite as written to favorites.json: only the known keys (never a password)
export function cleanFavorite(f) {
  const n = normalizeFavorite(f);
  return {
    id: n.id, name: String(n.name).trim(), group: n.group || null, color: Number.isInteger(n.color) ? n.color : null,
    type: n.type, host: String(n.host || '').trim(), port: Number(n.port) || 3306, user: String(n.user || ''), database: String(n.database || '').trim(),
    socket: String(n.socket || '').trim(),
    ssh: { host: String(n.ssh.host || '').trim(), port: Number(n.ssh.port) || 22, user: String(n.ssh.user || ''), key: String(n.ssh.key || '') },
    tls: { mode: TLS_MODES.includes(n.tls.mode) ? n.tls.mode : 'preferred', ca: String(n.tls.ca || ''), cert: String(n.tls.cert || ''), key: String(n.tls.key || '') },
    cloud: { service: String(n.cloud.service || ''), region: String(n.cloud.region || ''), iamProfile: String(n.cloud.iamProfile || '') },
    readOnly: !!n.readOnly, production: !!n.production, timeZone: String(n.timeZone || ''), trust: String(n.trust || ''),
    ...(n.imported ? { imported: { from: String(n.imported.from || ''), id: String(n.imported.id || '') } } : {}),
    quick: !!n.quick,
  };
}

// The groups in tree order with their depth: [{ group, depth }]
export function groupTree(groups) {
  const out = [];
  const kids = (parent) => (groups || []).filter((g) => (g.parent || null) === parent);
  const walk = (parent, depth, seen) => {
    for (const g of kids(parent)) {
      if (seen.has(g.id)) continue;
      seen.add(g.id);
      out.push({ group: g, depth });
      walk(g.id, depth + 1, seen);
    }
  };
  walk(null, 0, new Set());
  return out;
}

// Search in the favorites: by name, host, user or database
export function filterFavorites(list, query) {
  const q = String(query || '').trim().toLowerCase();
  if (!q) return list;
  return list.filter((f) => [f.name, f.host, f.user, f.database, f.socket].some((v) => String(v || '').toLowerCase().includes(q)));
}

// The recent connections kept in settings: newest first, at most max, one per favorite
export function pushRecent(list, favId, at = Date.now(), max = 8) {
  return [{ id: favId, at }, ...(list || []).filter((r) => r && r.id !== favId)].slice(0, max);
}
// query history per favorite: newest first, one copy of each text, at most max, each at most 20000 characters
export function pushHistory(list, sql, at = Date.now(), max = 60) {
  const s = String(sql || '').trim();
  if (!s || s.length > 20000) return list || [];
  return [{ sql: s, at }, ...(list || []).filter((h) => h && h.sql !== s)].slice(0, max);
}

// ---- formatting ----
export function humanBytes(n) {
  if (n === null || n === undefined || !Number.isFinite(Number(n))) return '—';
  const v = Number(n);
  if (v < 1024) return `${v} B`;
  const units = ['KiB', 'MiB', 'GiB', 'TiB'];
  let x = v / 1024, i = 0;
  while (x >= 1024 && i < units.length - 1) { x /= 1024; i++; }
  return `${x >= 100 ? Math.round(x) : Math.round(x * 10) / 10} ${units[i]}`;
}
export const fmtMs = (ms) => (ms === null || ms === undefined ? '' : ms < 10 ? `${Math.round(ms * 10) / 10} ms` : ms < 10000 ? `${Math.round(ms)} ms` : `${Math.round(ms / 100) / 10} s`);

// Column widths for a grid, from the names and a sample of the values (monospace-ish estimate)
export function columnWidths(columns, rows, { min = 60, max = 360, charW = 7.4, sample = 200 } = {}) {
  return columns.map((c, i) => {
    let w = String(c).length * (charW + 0.4) + 26;
    for (let r = 0; r < Math.min(rows.length, sample); r++) {
      const v = rows[r][i];
      const n = v === null || v === undefined ? 4 : Math.min(60, cellText(v, 60).length);
      w = Math.max(w, n * charW + 20);
    }
    return Math.round(Math.max(min, Math.min(max, w)));
  });
}
// columns that look numeric: right-aligned in the grids
export const NUMERIC_TYPE_NAMES = new Set(['TINY', 'SHORT', 'LONG', 'LONGLONG', 'INT24', 'FLOAT', 'DOUBLE', 'DECIMAL', 'NEWDECIMAL', 'TINYINT', 'SMALLINT', 'MEDIUMINT', 'INT', 'BIGINT', 'YEAR']);
export const isNumericColumn = (col) => !!col && NUMERIC_TYPE_NAMES.has(String(col.typeName || '').toUpperCase());

// lord-sql's error codes the app knows (each has databases.err.<code>)
export const ERROR_CODES = ['unavailable', 'notfound', 'dns', 'connect', 'tls', 'cert', 'auth', 'sql', 'closed', 'busy', 'toolarge', 'unsupported'];
export const errorCode = (e) => (ERROR_CODES.includes(e?.code) ? e.code : 'other');
