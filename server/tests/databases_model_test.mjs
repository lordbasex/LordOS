// Databases without a screen: quoting, literals, the Content filter and pages, row edits, Structure's parsers and ALTER,
// the Query editor's statements, safe mode, read-only, redaction, completion, Copy as and favorites.
// node server/tests/databases_model_test.mjs
import {
  quoteId, qualified, literal, escapeString, HEX_PREFIX, cellInput, cellText, splitList, ruleSql, whereSql, pageSql, countSql, pageCount, nextSort, pageRange,
  rowKey, updateCellSql, selectRowSql, deleteRowsSql, insertRowSql, duplicateValues, parseType, parseDefault, parseColumns, parseIndexes, parseStatus,
  parseCreate, parseTables, tablesSql, statusSql, validateField, columnDef, alterSql, alterClause, validateIndex, freeName, splitStatements, statementAt,
  offsetOf, posOf, stripComments, isDangerous, isReadOnlySql, redactSql, wordBefore, completions, columnsByTable, toCsv, toJson, toInserts,
  newFavoriteId, newGroupId, blankFavorite, normalizeFavorite, validateFavorite, cleanFavorite, colorOf, groupTree, filterFavorites, pushRecent, pushHistory,
  humanBytes, fmtMs, columnWidths, errorCode, lengthRule, firstWord,
} from '../../apps/databases/model.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}:\n  got  ${JSON.stringify(a)}\n  want ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };
const res = (names, rows) => ({ columns: names.map((name) => ({ name })), rows });

// ---- identifiers and literals ----
eq('quoteId', quoteId('orders'), '`orders`');
eq('quoteId backtick', quoteId('we`ird'), '`we``ird`');
eq('quoteId two', quoteId('a``b'), '`a````b`');
eq('qualified', qualified('shop', 'orders'), '`shop`.`orders`');
eq('qualified no db', qualified('', 'orders'), '`orders`');
eq('literal null', literal(null), 'NULL');
eq('literal undefined', literal(undefined), 'NULL');
eq('literal number', literal(42), '42');
eq('literal float', literal(-1.5), '-1.5');
eq('literal NaN', literal(NaN), 'NULL');
eq('literal bool', [literal(true), literal(false)], ['1', '0']);
eq('literal string', literal("it's"), "'it''s'");
eq('literal escapes', literal('a\0b\nc\rd\\e\'f"g\x1ah'), "'a\\0b\\nc\\rd\\\\e''f\"g\\Zh'");
eq('escapeString plain', escapeString('hello wörld'), 'hello wörld');
eq('literal hex', literal(`${HEX_PREFIX}deadBEEF`), "X'DEADBEEF'");
eq('literal empty hex', literal(HEX_PREFIX), "''");
let threw = false; try { literal(`${HEX_PREFIX}xyz`); } catch (e) { threw = e.message === 'badhex'; }
eq('literal bad hex throws', threw, true);
threw = false; try { literal(`${HEX_PREFIX}abc`); } catch (e) { threw = true; }
eq('literal odd hex throws', threw, true);
eq('literal digits stay quoted', literal('007'), "'007'");
eq('cellInput hex', cellInput('0xAB01', true), `${HEX_PREFIX}ab01`);
eq('cellInput text in binary', cellInput('hello', true), 'hello');
eq('cellInput not binary', cellInput('0xAB', false), '0xAB');
eq('cellText null', cellText(null), 'NULL');
eq('cellText hex', cellText(`${HEX_PREFIX}ab01`), '0xAB01');
eq('cellText newline', cellText('a\nb'), 'a ↵ b');
eq('cellText cut', cellText('abcdef', 3), 'abc…');

// ---- filter ----
eq('splitList', splitList("a, 'b,c', 3"), ['a', 'b,c', '3']);
eq('splitList doubled quote', splitList("'it''s', x"), ["it's", 'x']);
eq('splitList empty items', splitList('1,,2, '), ['1', '2']);
eq('splitList quoted empty', splitList("''"), ['']);
eq('splitList empty', splitList(''), []);
eq('rule =', ruleSql({ column: 'status', op: '=', value: 'paid' }), "`status` = 'paid'");
eq('rule !=', ruleSql({ column: 'n', op: '!=', value: '3' }), "`n` != '3'");
eq('rule >', ruleSql({ column: 'n', op: '>', value: '3' }), "`n` > '3'");
eq('rule <', ruleSql({ column: 'n', op: '<', value: '3' }), "`n` < '3'");
eq('rule contains', ruleSql({ column: 'notes', op: 'contains', value: '50%_off' }), "`notes` LIKE '%50\\\\%\\\\_off%'");
eq('rule begins', ruleSql({ column: 'name', op: 'begins', value: 'Ac' }), "`name` LIKE 'Ac%'");
eq('rule ends', ruleSql({ column: 'name', op: 'ends', value: "o'" }), "`name` LIKE '%o'''");
eq('rule not contains', ruleSql({ column: 'a', op: 'notContains', value: 'x' }), "`a` NOT LIKE '%x%'");
eq('rule like backslash', ruleSql({ column: 'p', op: 'contains', value: 'C:\\' }), "`p` LIKE '%C:\\\\\\\\%'");
eq('rule is null', ruleSql({ column: 'notes', op: 'isNull' }), '`notes` IS NULL');
eq('rule not null', ruleSql({ column: 'notes', op: 'notNull', value: 'ignored' }), '`notes` IS NOT NULL');
eq('rule in', ruleSql({ column: 'id', op: 'in', value: "1, 2, 'x,y'" }), "`id` IN ('1', '2', 'x,y')");
eq('rule in empty', ruleSql({ column: 'id', op: 'in', value: ' ' }), null);
eq('rule between', ruleSql({ column: 'total', op: 'between', value: '10', value2: '20' }), "`total` BETWEEN '10' AND '20'");
eq('rule bad op', ruleSql({ column: 'a', op: 'DROP', value: 'x' }), null);
eq('rule no column', ruleSql({ op: '=', value: 'x' }), null);
eq('rule column quoted', ruleSql({ column: 'a`b', op: '=', value: '1' }), "`a``b` = '1'");
eq('where none', whereSql([]), '');
eq('where and', whereSql([{ column: 'a', op: '=', value: '1' }, { column: 'b', op: 'isNull' }, { column: 'c', op: '=', value: 'x', enabled: false }]), " WHERE `a` = '1' AND `b` IS NULL");
eq('page 0', pageSql({ db: 'shop', table: 'orders', rules: [], sort: null, page: 0 }), 'SELECT * FROM `shop`.`orders` LIMIT 1000');
eq('page 2 sorted', pageSql({ db: 'shop', table: 'orders', rules: [{ column: 'status', op: '=', value: 'paid' }], sort: { column: 'created_at', dir: 'desc' }, page: 2 }),
  "SELECT * FROM `shop`.`orders` WHERE `status` = 'paid' ORDER BY `created_at` DESC LIMIT 1000 OFFSET 2000");
eq('page negative', pageSql({ db: 'd', table: 't', page: -3 }), 'SELECT * FROM `d`.`t` LIMIT 1000');
eq('count', countSql({ db: 'd', table: 't', rules: [{ column: 'a', op: 'isNull' }] }), 'SELECT COUNT(*) FROM `d`.`t` WHERE `a` IS NULL');
eq('pageCount', [pageCount(0), pageCount(1000), pageCount(1001), pageCount(7214)], [1, 1, 2, 8]);
eq('nextSort', [nextSort(null, 'a'), nextSort({ column: 'a', dir: 'asc' }, 'a'), nextSort({ column: 'a', dir: 'desc' }, 'a'), nextSort({ column: 'a', dir: 'desc' }, 'b')],
  [{ column: 'a', dir: 'asc' }, { column: 'a', dir: 'desc' }, null, { column: 'b', dir: 'asc' }]);
eq('pageRange', [pageRange(0, 1000), pageRange(7, 214), pageRange(0, 0)], [[1, 1000], [7001, 7214], [0, 0]]);

// ---- structure parsers ----
const COLS = res(['Field', 'Type', 'Collation', 'Null', 'Key', 'Default', 'Extra', 'Privileges', 'Comment'], [
  ['id', 'int(11) unsigned', null, 'NO', 'PRI', null, 'auto_increment', 'select', ''],
  ['customer_id', 'int(11) unsigned', null, 'NO', 'MUL', null, '', 'select', ''],
  ['status', "enum('pending','paid','it''s')", 'utf8mb4_unicode_ci', 'NO', 'MUL', "'pending'", '', 'select', ''],
  ['total', 'decimal(10,2) unsigned zerofill', null, 'NO', '', '0.00', '', 'select', 'sum'],
  ['notes', 'text', 'utf8mb4_unicode_ci', 'YES', '', 'NULL', '', 'select', ''],
  ['created_at', 'datetime', null, 'NO', '', 'current_timestamp()', '', 'select', ''],
  ['updated_at', 'datetime', null, 'YES', '', null, 'on update current_timestamp()', 'select', ''],
  ['code', 'char(3)', 'utf8mb4_bin', 'NO', 'UNI', 'USD', '', 'select', ''],
]);
const fields = parseColumns(COLS);
eq('fields count', fields.length, 8);
eq('field id', [fields[0].type, fields[0].length, fields[0].unsigned, fields[0].autoIncrement, fields[0].key, fields[0].def.kind], ['INT', '11', true, true, 'PRI', 'none']);
eq('field enum', [fields[2].type, fields[2].length, fields[2].def], ['ENUM', "'pending','paid','it''s'", { kind: 'value', value: 'pending' }]);
eq('field decimal', [fields[3].type, fields[3].length, fields[3].unsigned, fields[3].zerofill, fields[3].def, fields[3].comment], ['DECIMAL', '10,2', true, true, { kind: 'value', value: '0.00' }, 'sum']);
eq('field null default (MariaDB word)', [fields[4].nullable, fields[4].def.kind], [true, 'null']);
eq('field expr default', fields[5].def, { kind: 'expr', value: 'current_timestamp()' });
eq('field null default (MySQL)', fields[6].def.kind, 'null');
eq('field on update', fields[6].onUpdate, 'current_timestamp()');
eq('field bare default', fields[7].def, { kind: 'value', value: 'USD' });
eq('parseType plain', parseType('json'), { type: 'JSON', length: '', unsigned: false, zerofill: false, binary: false });
eq('parseType set', parseType("set('a','b')").length, "'a','b'");
eq('parseType paren in enum', parseType("enum('a)','b')").length, "'a)','b'");
eq('parseDefault quoted quote', parseDefault("'it''s'", false), { kind: 'value', value: "it's" });
eq('parseDefault NOT NULL string NULL', parseDefault('NULL', false), { kind: 'value', value: 'NULL' });
eq('parseDefault now', parseDefault('CURRENT_TIMESTAMP', false).kind, 'expr');

const IDX = res(['Table', 'Non_unique', 'Key_name', 'Seq_in_index', 'Column_name', 'Collation', 'Cardinality', 'Sub_part', 'Packed', 'Null', 'Index_type', 'Comment', 'Index_comment'], [
  ['orders', '1', 'idx_status_created', '2', 'created_at', 'A', '9840', null, null, '', 'BTREE', '', 'dashboard filter'],
  ['orders', '0', 'PRIMARY', '1', 'id', 'A', '12480', null, null, '', 'BTREE', '', ''],
  ['orders', '1', 'idx_status_created', '1', 'status', 'A', '12', null, null, '', 'BTREE', '', 'dashboard filter'],
  ['orders', '0', 'uq_code', '1', 'code', 'A', null, null, null, '', 'BTREE', '', ''],
  ['orders', '1', 'ft_notes', '1', 'notes', null, null, '10', null, 'YES', 'FULLTEXT', '', ''],
]);
const indexes = parseIndexes(IDX);
eq('indexes order', indexes.map((x) => x.name), ['PRIMARY', 'idx_status_created', 'uq_code', 'ft_notes']);
eq('index columns sorted', indexes[1].columns.map((c) => c.name), ['status', 'created_at']);
eq('index unique', indexes.map((x) => x.unique), [true, false, true, false]);
eq('index cardinality', [indexes[0].cardinality, indexes[1].cardinality, indexes[2].cardinality], [12480, 9840, null]);
eq('index sub part', indexes[3].columns[0].sub, 10);
eq('index comment', indexes[1].comment, 'dashboard filter');

const STATUS = res(['Name', 'Engine', 'Version', 'Row_format', 'Rows', 'Avg_row_length', 'Data_length', 'Max_data_length', 'Index_length', 'Data_free', 'Auto_increment', 'Create_time', 'Update_time', 'Check_time', 'Collation', 'Checksum', 'Create_options', 'Comment'], [
  ['orders', 'InnoDB', '10', 'Dynamic', '12480', '352', '4403200', '0', '3040870', '4194304', '12541', '2025-03-02 10:11:04', '2026-11-08 21:15:12', null, 'utf8mb4_unicode_ci', null, '', 'One row per order'],
]);
const st = parseStatus(STATUS);
eq('status', [st.engine, st.rows, st.data, st.autoIncrement, st.charset, st.comment, st.view], ['InnoDB', 12480, 4403200, 12541, 'utf8mb4', 'One row per order', false]);
eq('status view', parseStatus(res(['Name', 'Engine', 'Comment'], [['v', null, 'VIEW']])).view, true);
eq('status none', parseStatus(res(['Name'], [])), null);
eq('create', parseCreate(res(['Table', 'Create Table'], [['orders', 'CREATE TABLE `orders` (...)']])), 'CREATE TABLE `orders` (...)');
eq('create view', parseCreate(res(['View', 'Create View', 'character_set_client'], [['v', 'CREATE VIEW v AS SELECT 1', 'utf8']])), 'CREATE VIEW v AS SELECT 1');
eq('tables', parseTables({ rows: [['orders', 'BASE TABLE'], ['v_sales', 'VIEW'], ['Audit', 'BASE TABLE'], ['b', 'SYSTEM VIEW']] }), { tables: ['Audit', 'orders'], views: ['b', 'v_sales'] });
eq('tablesSql', tablesSql('my`db'), 'SHOW FULL TABLES FROM `my``db`');
eq('statusSql escapes LIKE', statusSql('shop', 'order_items'), "SHOW TABLE STATUS FROM `shop` LIKE 'order\\\\_items'");

// ---- row edits ----
const columns = ['id', 'customer_id', 'status', 'total', 'currency', 'notes', 'created_at', 'updated_at'];
const row = ['12533', '3877', 'paid', '1240.00', 'EUR', 'Invoice', '2026-11-08 20:31:09', null];
const key = rowKey(fields, indexes);
eq('rowKey primary', key, { cols: ['id'] });
eq('update cell', updateCellSql({ db: 'shop', table: 'orders', key, columns, row, column: 'notes', value: "Acme's" }),
  { sql: "UPDATE `shop`.`orders` SET `notes` = 'Acme''s' WHERE `id` = '12533' LIMIT 1" });
eq('update cell null', updateCellSql({ db: 'shop', table: 'orders', key, columns, row, column: 'notes', value: null }).sql, "UPDATE `shop`.`orders` SET `notes` = NULL WHERE `id` = '12533' LIMIT 1");
eq('update no key', updateCellSql({ db: 'shop', table: 'orders', key: { error: 'nokey' }, columns, row, column: 'notes', value: 'x' }), { error: 'nokey' });
eq('update bad hex', updateCellSql({ db: 'd', table: 't', key, columns, row, column: 'notes', value: `${HEX_PREFIX}zz` }), { error: 'badhex' });
eq('update key column missing', updateCellSql({ db: 'd', table: 't', key: { cols: ['nope'] }, columns, row, column: 'notes', value: 'x' }), { error: 'nokey' });
const ukey = rowKey(fields, indexes.filter((x) => x.name !== 'PRIMARY'));
eq('rowKey unique not null', ukey, { cols: ['code'] });
eq('rowKey unique nullable refused', rowKey([{ name: 'a', nullable: true }], [{ name: 'u', unique: true, columns: [{ name: 'a' }] }]), { error: 'nokey' });
eq('rowKey prefix unique refused', rowKey([{ name: 'a', nullable: false }], [{ name: 'u', unique: true, columns: [{ name: 'a', sub: 10 }] }]), { error: 'nokey' });
eq('rowKey none', rowKey([{ name: 'a', nullable: false }], []), { error: 'nokey' });
eq('rowKey view', rowKey(fields, indexes, { view: true }), { error: 'view' });
eq('rowKey composite', rowKey(fields, [{ name: 'PRIMARY', unique: true, columns: [{ name: 'id' }, { name: 'customer_id' }] }]), { cols: ['id', 'customer_id'] });
eq('update composite', updateCellSql({ db: 'd', table: 't', key: { cols: ['id', 'updated_at'] }, columns, row, column: 'notes', value: 'x' }).sql,
  "UPDATE `d`.`t` SET `notes` = 'x' WHERE `id` = '12533' AND `updated_at` IS NULL LIMIT 1");
eq('select row', selectRowSql({ db: 'd', table: 't', key, columns, row }).sql, "SELECT * FROM `d`.`t` WHERE `id` = '12533' LIMIT 1");
eq('delete one', deleteRowsSql({ db: 'd', table: 't', key, columns, rows: [row] }).sql, "DELETE FROM `d`.`t` WHERE `id` = '12533' LIMIT 1");
eq('delete many', deleteRowsSql({ db: 'd', table: 't', key, columns, rows: [row, ['7', ...row.slice(1)]] }).sql, "DELETE FROM `d`.`t` WHERE `id` IN ('12533', '7') LIMIT 2");
eq('delete composite', deleteRowsSql({ db: 'd', table: 't', key: { cols: ['id', 'currency'] }, columns, rows: [row, ['8', ...row.slice(1)]] }).sql,
  "DELETE FROM `d`.`t` WHERE (`id` = '12533' AND `currency` = 'EUR') OR (`id` = '8' AND `currency` = 'EUR') LIMIT 2");
eq('delete none', deleteRowsSql({ db: 'd', table: 't', key, columns, rows: [] }), { error: 'norows' });
eq('delete no key', deleteRowsSql({ db: 'd', table: 't', key: { error: 'nokey' }, columns, rows: [row] }), { error: 'nokey' });
eq('insert', insertRowSql({ db: 'd', table: 't', values: { a: '1', b: null, c: undefined } }).sql, "INSERT INTO `d`.`t` (`a`, `b`) VALUES ('1', NULL)");
eq('insert defaults', insertRowSql({ db: 'd', table: 't', values: {} }).sql, 'INSERT INTO `d`.`t` () VALUES ()');
eq('duplicate skips auto inc', Object.keys(duplicateValues(fields, columns, row)), columns.slice(1));

// ---- ALTER ----
const status = { ...fields[2], length: "'pending','paid','shipped'" };
eq('columnDef enum', columnDef(status), "`status` ENUM('pending','paid','shipped') COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT 'pending'");
eq('columnDef id', columnDef(fields[0]), '`id` INT(11) UNSIGNED NOT NULL AUTO_INCREMENT');
eq('columnDef decimal', columnDef(fields[3]), "`total` DECIMAL(10,2) UNSIGNED ZEROFILL NOT NULL DEFAULT 0.00 COMMENT 'sum'");
eq('columnDef null default', columnDef(fields[4]), '`notes` TEXT COLLATE utf8mb4_unicode_ci NULL DEFAULT NULL');
eq('columnDef expr', columnDef(fields[5]), '`created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP()');
eq('columnDef on update', columnDef(fields[6]), '`updated_at` DATETIME NULL DEFAULT NULL ON UPDATE CURRENT_TIMESTAMP()');
eq('columnDef unsigned ignored on text', columnDef({ name: 'x', type: 'varchar', length: '20', unsigned: true, nullable: true, def: { kind: 'value', value: "a'b" } }),
  "`x` VARCHAR(20) NULL DEFAULT 'a''b'");
eq('alter modify', alterSql('shop', 'orders', [{ op: 'change', from: 'status', field: status }]),
  "ALTER TABLE `shop`.`orders`\n  MODIFY `status` ENUM('pending','paid','shipped') COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT 'pending'");
eq('alter rename', alterClause({ op: 'change', from: 'notes', field: { ...fields[4], name: 'note' } }), 'CHANGE `notes` `note` TEXT COLLATE utf8mb4_unicode_ci NULL DEFAULT NULL');
eq('alter add after', alterClause({ op: 'add', field: { name: 'flag', type: 'TINYINT', length: '1', nullable: false, def: { kind: 'value', value: '0' } }, after: 'total' }),
  'ADD `flag` TINYINT(1) NOT NULL DEFAULT 0 AFTER `total`');
eq('alter add first', alterClause({ op: 'add', field: { name: 'f', type: 'INT', nullable: true, def: { kind: 'null' } }, after: null }), 'ADD `f` INT NULL DEFAULT NULL FIRST');
eq('alter drop', alterClause({ op: 'drop', name: 'we`ird' }), 'DROP `we``ird`');
eq('alter add index', alterClause({ op: 'addIndex', index: { name: 'idx_a', kind: 'INDEX', columns: [{ name: 'a' }, { name: 'b', sub: 10 }] } }), 'ADD INDEX `idx_a` (`a`, `b`(10))');
eq('alter add unique', alterClause({ op: 'addIndex', index: { name: 'u', kind: 'UNIQUE', columns: [{ name: 'a' }] } }), 'ADD UNIQUE INDEX `u` (`a`)');
eq('alter add fulltext', alterClause({ op: 'addIndex', index: { name: 'f', kind: 'FULLTEXT', columns: [{ name: 'a' }] } }), 'ADD FULLTEXT INDEX `f` (`a`)');
eq('alter add primary', alterClause({ op: 'addIndex', index: { kind: 'PRIMARY', columns: [{ name: 'id' }] } }), 'ADD PRIMARY KEY (`id`)');
eq('alter drop index', alterClause({ op: 'dropIndex', name: 'idx_a' }), 'DROP INDEX `idx_a`');
eq('alter drop primary', alterClause({ op: 'dropIndex', name: 'PRIMARY' }), 'DROP PRIMARY KEY');
eq('alter several', alterSql('d', 't', [{ op: 'drop', name: 'a' }, { op: 'dropIndex', name: 'b' }]), 'ALTER TABLE `d`.`t`\n  DROP `a`,\n  DROP INDEX `b`');
eq('alter none', alterSql('d', 't', []), '');
eq('validate ok', validateField(fields[0]), []);
eq('validate enum ok', validateField(status), []);
eq('validate empty name', validateField({ ...fields[0], name: '' }).map((p) => p.code), ['name']);
eq('validate varchar needs length', validateField({ name: 'x', type: 'VARCHAR', length: '', nullable: true, def: { kind: 'null' } }).map((p) => p.code), ['lengthNeeded']);
eq('validate json no length', validateField({ name: 'x', type: 'JSON', length: '10', nullable: true, def: { kind: 'none' } }).map((p) => p.code), ['lengthNone']);
eq('validate bad length', validateField({ name: 'x', type: 'INT', length: '1); DROP', nullable: true, def: { kind: 'none' } }).map((p) => p.code), ['length']);
eq('validate bad enum', validateField({ name: 'x', type: 'ENUM', length: "a','b", nullable: true, def: { kind: 'none' } }).map((p) => p.code), ['enumValues']);
eq('validate decimal', validateField({ name: 'x', type: 'DECIMAL', length: '10, 2', nullable: true, def: { kind: 'none' } }), []);
eq('validate null default on not null', validateField({ name: 'x', type: 'INT', nullable: false, def: { kind: 'null' } }).map((p) => p.code), ['nullDefault']);
eq('validate text default', validateField({ name: 'x', type: 'TEXT', nullable: true, def: { kind: 'value', value: 'a' } }).map((p) => p.code), ['noDefault']);
eq('validate auto inc varchar', validateField({ name: 'x', type: 'VARCHAR', length: '3', nullable: false, def: { kind: 'none' }, autoIncrement: true }).map((p) => p.code), ['autoIncrement']);
eq('validate bad type', validateField({ name: 'x', type: 'INT; DROP', nullable: true, def: { kind: 'none' } }).map((p) => p.code), ['type']);
eq('validate collation', validateField({ name: 'x', type: 'TEXT', nullable: true, def: { kind: 'none' }, collation: 'utf8 x' }).map((p) => p.code), ['collation']);
eq('lengthRule', [lengthRule('varchar'), lengthRule('int'), lengthRule('json')], ['required', 'optional', 'none']);
eq('validateIndex ok', validateIndex({ name: 'i', kind: 'INDEX', columns: [{ name: 'id' }] }, fields), []);
eq('validateIndex no columns', validateIndex({ name: 'i', kind: 'INDEX', columns: [] }, fields).map((p) => p.code), ['columns']);
eq('validateIndex unknown column', validateIndex({ name: 'i', kind: 'INDEX', columns: [{ name: 'nope' }] }, fields).map((p) => p.code), ['columns']);
eq('validateIndex PRIMARY name', validateIndex({ name: 'PRIMARY', kind: 'INDEX', columns: [{ name: 'id' }] }, fields).map((p) => p.code), ['name']);
eq('freeName', [freeName('field', []), freeName('field', ['field', 'field_2'])], ['field', 'field_3']);

// ---- statements ----
const sql = "-- top customers\nSELECT 1;\nSELECT 'a;b', \"c;d\", `e;f` FROM t; # x;\nUPDATE t SET a = 'it''s;' WHERE id = 1;\n/* c; */ DELETE FROM t\n";
const parts = splitStatements(sql);
eq('split count', parts.length, 4);
eq('split texts', parts.map((p) => p.sql), ['-- top customers\nSELECT 1', "SELECT 'a;b', \"c;d\", `e;f` FROM t", "# x;\nUPDATE t SET a = 'it''s;' WHERE id = 1", '/* c; */ DELETE FROM t']);
eq('split offsets', parts.map((p) => sql.slice(p.start, p.end)), parts.map((p) => p.sql));
eq('split backslash quote', splitStatements("SELECT 'a\\';b'; SELECT 2").map((p) => p.sql), ["SELECT 'a\\';b'", 'SELECT 2']);
eq('split dash no space is not comment', splitStatements('SELECT 1--1; SELECT 2').map((p) => p.sql), ['SELECT 1--1', 'SELECT 2']);
eq('split comment only skipped', splitStatements('SELECT 1; -- just a note\n;  ;').map((p) => p.sql), ['SELECT 1']);
const proc = 'DELIMITER //\nCREATE PROCEDURE p() BEGIN SELECT 1; SELECT 2; END//\nDELIMITER ;\nSELECT 3;';
eq('split delimiter', splitStatements(proc).map((p) => p.sql), ['CREATE PROCEDURE p() BEGIN SELECT 1; SELECT 2; END', 'SELECT 3']);
eq('split delimiter $$', splitStatements('delimiter $$\nSELECT 1$$ SELECT 2$$').map((p) => p.sql), ['SELECT 1', 'SELECT 2']);
eq('split unterminated quote', splitStatements("SELECT 'abc; SELECT 2").length, 1);
eq('split empty', splitStatements(''), []);
const buf = 'SELECT 1;\n\nSELECT 2;   \nUPDATE t SET a=1;';
eq('at in first', statementAt(buf, 3).sql, 'SELECT 1');
eq('at after delimiter same line', statementAt(buf, 9).sql, 'SELECT 1');
eq('at blank line picks previous', statementAt(buf, 10).sql, 'SELECT 1');
eq('at second', statementAt(buf, 14).sql, 'SELECT 2');
eq('at trailing spaces', statementAt(buf, 22).sql, 'SELECT 2');
eq('at last', statementAt(buf, buf.length).sql, 'UPDATE t SET a=1');
eq('at before any', statementAt('\n\nSELECT 9', 0).sql, 'SELECT 9');
eq('at none', statementAt('  ', 1), null);
const lines = ['ab', 'cde', 'f'];
eq('offsetOf', offsetOf(lines, { l: 1, c: 2 }), 5);
eq('posOf', posOf('ab\ncde\nf', 5), { l: 1, c: 2 });
eq('stripComments keeps strings', stripComments("SELECT '--x' -- y\n, 1 /* z */"), "SELECT '--x'  \n, 1  ");
eq('stripComments executable', stripComments('SELECT /*!40001 SQL_NO_CACHE */ 1').replace(/\s+/g, ' '), 'SELECT SQL_NO_CACHE 1');

// ---- safe mode and read only ----
eq('dangerous update', isDangerous('UPDATE orders SET status = 1'), true);
eq('dangerous delete', isDangerous('delete from orders'), true);
eq('not dangerous where', isDangerous('UPDATE orders SET a = 1 WHERE id = 3'), false);
eq('where in string does not count', isDangerous("UPDATE t SET note = 'WHERE'"), true);
eq('where in comment does not count', isDangerous('DELETE FROM t -- WHERE id = 1'), true);
eq('where in ident does not count', isDangerous('DELETE FROM `where`'), true);
eq('select not dangerous', isDangerous('SELECT * FROM t'), false);
eq('multi-table delete with where', isDangerous('DELETE t1 FROM t1 JOIN t2 ON t1.a = t2.a WHERE t2.b = 1'), false);
eq('cte delete', isDangerous('WITH x AS (SELECT 1) DELETE FROM t'), true);
eq('leading comment', isDangerous('/* note */ UPDATE t SET a = 1'), true);
eq('firstWord', [firstWord('  (SELECT 1)'), firstWord('-- c\nshow tables'), firstWord('')], ['SELECT', 'SHOW', '']);
eq('read select', isReadOnlySql('SELECT * FROM t'), true);
eq('read show', isReadOnlySql('show databases'), true);
eq('read explain', isReadOnlySql('EXPLAIN SELECT 1'), true);
eq('read paren select', isReadOnlySql('(SELECT 1) UNION (SELECT 2)'), true);
eq('read with select', isReadOnlySql('WITH a AS (SELECT 1) SELECT * FROM a'), true);
eq('not read with delete', isReadOnlySql('WITH a AS (SELECT 1) DELETE FROM t'), false);
eq('not read update', isReadOnlySql('UPDATE t SET a = 1 WHERE b = 2'), false);
eq('not read into outfile', isReadOnlySql("SELECT * FROM t INTO OUTFILE '/tmp/x'"), false);
eq('not read for update', isReadOnlySql('SELECT * FROM t FOR UPDATE'), false);
eq('not read drop', isReadOnlySql('DROP TABLE t'), false);
eq('not read set', isReadOnlySql('SET GLOBAL x = 1'), false);
eq('read string with update', isReadOnlySql("SELECT 'UPDATE t' AS x"), true);
eq('not read analyze table', isReadOnlySql('ANALYZE TABLE t'), false);
eq('not read explain analyze update', isReadOnlySql('EXPLAIN ANALYZE UPDATE t SET a = 1'), false);

// ---- redaction ----
eq('redact identified', redactSql("CREATE USER 'a'@'%' IDENTIFIED BY 's3cr;et'"), "CREATE USER 'a'@'%' IDENTIFIED BY '••••'");
eq('redact with plugin', redactSql("ALTER USER x IDENTIFIED WITH mysql_native_password BY \"pw\""), "ALTER USER x IDENTIFIED WITH mysql_native_password BY '••••'");
eq('redact password()', redactSql("SET PASSWORD FOR x = PASSWORD('pw')"), "SET PASSWORD FOR x = PASSWORD('••••')");
eq('redact set password =', redactSql("SET PASSWORD = 'pw'"), "SET PASSWORD = '••••'");
eq('redact master', redactSql("CHANGE MASTER TO MASTER_PASSWORD='pw'"), "CHANGE MASTER TO MASTER_PASSWORD='••••'");
eq('redact escaped quote', redactSql("CREATE USER a IDENTIFIED BY 'p\\'w'"), "CREATE USER a IDENTIFIED BY '••••'");
eq('redact leaves others', redactSql("SELECT 'password'"), "SELECT 'password'");

// ---- completion ----
eq('wordBefore', wordBefore('  AND o.cr', 10), { word: 'cr', from: 8, qualifier: 'o' });
eq('wordBefore plain', wordBefore('SELECT na', 9), { word: 'na', from: 7, qualifier: '' });
eq('wordBefore empty', wordBefore('x = ', 4), { word: '', from: 4, qualifier: '' });
const schema = { tables: ['customers', 'orders'], columns: { orders: [{ name: 'created_at', type: 'DATETIME' }, { name: 'currency', type: 'CHAR(3)' }], customers: [{ name: 'city', type: 'VARCHAR(40)' }] } };
const text = 'SELECT * FROM customers c JOIN orders o ON o.customer_id = c.id WHERE o.cr';
eq('complete alias', completions('cr', schema, { qualifier: 'o', text }).map((c) => c.label), ['created_at']);
eq('complete alias all', completions('', schema, { qualifier: 'o', text }).map((c) => c.label), ['created_at', 'currency']);
eq('complete table qualifier', completions('ci', schema, { qualifier: 'customers', text: '' }).map((c) => c.label), ['city']);
eq('complete cu', completions('cu', schema, { text }).map((c) => c.label), ['currency', 'customers', 'CURRENT_DATE', 'CURRENT_TIMESTAMP']);
eq('complete kinds', completions('cu', schema, { text }).map((c) => c.kind), ['column', 'table', 'function', 'function']);
eq('complete keyword', completions('sel', schema, { text: '' }).map((c) => c.label), ['SELECT']);
eq('complete exact skipped', completions('orders', schema, { text: '' }).map((c) => c.label), []);
eq('complete empty', completions('', schema, { text }), []);
eq('columnsByTable', columnsByTable({ rows: [['t', 'a', 'int(11)'], ['t', 'b', 'text'], ['u', 'c', 'json']] }), { t: [{ name: 'a', type: 'INT(11)' }, { name: 'b', type: 'TEXT' }], u: [{ name: 'c', type: 'JSON' }] });

// ---- copy as ----
eq('csv', toCsv(['a', 'b'], [['1', 'x,y'], [null, 'q"']]), 'a,b\n1,"x,y"\nNULL,"q"""');
eq('json', JSON.parse(toJson(['a', 'b'], [['1', null], [`${HEX_PREFIX}ff`, 'x']])), [{ a: '1', b: null }, { a: '0xff', b: 'x' }]);
eq('inserts', toInserts('t', ['a', 'b'], [['1', null], ["o'k", 'x']]), "INSERT INTO `t` (`a`, `b`) VALUES\n  ('1', NULL),\n  ('o''k', 'x');");
eq('inserts none', toInserts('t', ['a'], []), '');

// ---- favorites ----
eq('new id', newFavoriteId('Prod Shop', []), 'prod-shop');
eq('new id taken', newFavoriteId('prod-shop', ['prod-shop', 'prod-shop-2']), 'prod-shop-3');
eq('new id accents', newFavoriteId('Tienda Año', []), 'tienda-ano');
eq('new id empty', newFavoriteId('  !!  ', []), 'connection');
eq('new id long', newFavoriteId('x'.repeat(100), []).length, 48);
eq('new group id', newGroupId(['g1', 'g2']), 'g3');
const fav = blankFavorite({ id: 'prod-shop', name: 'prod-shop', host: 'db.example.net', user: 'shop_admin' });
eq('valid favorite', validateFavorite(fav), []);
eq('favorite needs name', validateFavorite({ ...fav, name: ' ' }).map((p) => p.field), ['name']);
eq('favorite bad host', validateFavorite({ ...fav, host: 'db example' }).map((p) => p.field), ['host']);
eq('favorite ipv6', validateFavorite({ ...fav, host: '::1' }), []);
eq('favorite ip', validateFavorite({ ...fav, host: '192.168.1.40' }), []);
eq('favorite bad port', validateFavorite({ ...fav, port: 70000 }).map((p) => p.field), ['port']);
eq('favorite empty port', validateFavorite({ ...fav, port: '' }).map((p) => p.field), ['port']);
eq('favorite socket', validateFavorite({ ...fav, type: 'socket', host: '', socket: '/run/mysqld/mysqld.sock' }), []);
eq('favorite socket relative', validateFavorite({ ...fav, type: 'socket', socket: 'mysqld.sock' }).map((p) => p.field), ['socket']);
eq('favorite ssh host', validateFavorite({ ...fav, type: 'ssh' }).map((p) => p.field), ['sshHost']);
eq('favorite bad id', validateFavorite({ ...fav, id: '../x' }).map((p) => p.field), ['id']);
eq('favorite id taken', validateFavorite(fav, [{ id: 'prod-shop' }]).map((p) => p.code), ['idTaken']);
eq('favorite cert pair', validateFavorite({ ...fav, tls: { mode: 'required', cert: '/a.pem', key: '' } }).map((p) => p.code), ['certPair']);
eq('favorite bad color', validateFavorite({ ...fav, color: 9 }).map((p) => p.field), ['color']);
eq('favorite bad tls', validateFavorite({ ...fav, tls: { mode: 'maybe' } }).map((p) => p.field), ['tls']);
eq('colorOf', [colorOf({ color: 0 }), colorOf({ color: null }), colorOf({ color: 7 })], ['#ff453a', null, null]);
eq('normalize fills', normalizeFavorite({ name: 'x', ssh: { host: 'h' } }).ssh, { host: 'h', port: 22, user: '', key: '' });
const cleaned = cleanFavorite({ ...fav, password: 'secret', ssh: { ...fav.ssh, password: 'x' }, port: '3307', imported: { from: 'sequel-ace', id: '12', pw: 'x' } });
eq('clean no password', JSON.stringify(cleaned).includes('secret') || JSON.stringify(cleaned).includes('password') || 'pw' in cleaned.imported, false);
eq('clean port number', cleaned.port, 3307);
eq('clean keys', Object.keys(cleaned), ['id', 'name', 'group', 'color', 'type', 'host', 'port', 'user', 'database', 'socket', 'ssh', 'tls', 'cloud', 'readOnly', 'production', 'timeZone', 'trust', 'imported', 'quick']);
eq('group tree', groupTree([{ id: 'b', parent: 'a', name: 'B' }, { id: 'a', parent: null, name: 'A' }, { id: 'c', parent: null, name: 'C' }, { id: 'x', parent: 'x', name: 'loop' }]).map((n) => `${n.group.id}${n.depth}`), ['a0', 'b1', 'c0']);
eq('filter favorites', filterFavorites([fav, { ...fav, id: 'b', name: 'local', host: '127.0.0.1', user: 'root' }], 'ROOT').map((f) => f.id), ['b']);
eq('filter favorites empty', filterFavorites([fav], '').length, 1);
eq('recent', pushRecent([{ id: 'a', at: 1 }, { id: 'b', at: 2 }], 'b', 5, 2), [{ id: 'b', at: 5 }, { id: 'a', at: 1 }]);
eq('recent max', pushRecent([{ id: 'a', at: 1 }, { id: 'b', at: 2 }], 'c', 5, 2).map((r) => r.id), ['c', 'a']);
eq('history dedupe', pushHistory([{ sql: 'SELECT 1', at: 1 }, { sql: 'SELECT 2', at: 2 }], ' SELECT 2 ', 9).map((h) => h.sql), ['SELECT 2', 'SELECT 1']);
eq('history empty', pushHistory([{ sql: 'a', at: 1 }], '  ').length, 1);
eq('history too long', pushHistory([], 'x'.repeat(20001)).length, 0);

// ---- formatting ----
eq('humanBytes', [humanBytes(512), humanBytes(4403200), humanBytes(null), humanBytes(1024 * 1024 * 1024 * 300)], ['512 B', '4.2 MiB', '—', '300 GiB']);
eq('fmtMs', [fmtMs(4.27), fmtMs(42.4), fmtMs(12345)], ['4.3 ms', '42 ms', '12.3 s']);
eq('widths', columnWidths(['id', 'a long column name'], [['1', 'x']]).length, 2);
eq('widths bounds', columnWidths(['x'], [['y'.repeat(500)]], { max: 200 })[0], 200);
eq('errorCode', [errorCode({ code: 'auth' }), errorCode({ code: 'weird' }), errorCode(null)], ['auth', 'other', 'other']);


// security scan F4: a stored value can't end the literal early, with or without NO_BACKSLASH_ESCAPES
// (a literal read the way the server reads it: in NBE mode a backslash is an ordinary character)
function readLiteral(sql, nbe) {
  if (sql[0] !== "'") return -1;
  for (let i = 1; i < sql.length; i++) {
    if (!nbe && sql[i] === '\\') { i++; continue; }
    if (sql[i] === "'") { if (sql[i + 1] === "'") { i++; continue; } return i + 1; }
  }
  return -1;
}
for (const v of ["x\\'; DROP TABLE accounts; -- ", "\\", "'", "a\\''b", "\\'\\'"]) {
  const l = literal(v);
  eq(`F4 literal is whole (default mode): ${JSON.stringify(v)}`, readLiteral(l, false), l.length);
  eq(`F4 literal is whole (NO_BACKSLASH_ESCAPES): ${JSON.stringify(v)}`, readLiteral(l, true), l.length);
}
// security scan F3: table names from the server never become patterns
{
  const evil = { tables: ['(.+)+!', '(', 'orders'], columns: { orders: [{ name: 'id', type: 'int' }], '(.+)+!': [{ name: 'boom', type: 'int' }] } };
  const t0 = Date.now();
  const got = completions('i', evil, { text: 'SELECT * FROM orders WHERE aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' }).map((c) => c.label);
  eq('F3 completion with hostile table names is quick', Date.now() - t0 < 200, true);
  eq('F3 completion still finds the used table', got.includes('id'), true);
  eq('F3 a table named "(" does not throw', Array.isArray(completions('o', evil, { text: '(' })), true);
  eq('F3 alias with a hostile qualifier does not throw', Array.isArray(completions('', evil, { qualifier: '(.+)+', text: 'FROM orders o' })), true);
}

console.log(`databases model: ${ok} checks${process.exitCode ? ', FAILED' : ' passed'}`);
