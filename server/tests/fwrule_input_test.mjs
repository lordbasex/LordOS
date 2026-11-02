// fwrule_input_test.mjs — the Settings / Monitor rule sheet's parser (sdk/fwrule.js).
//
//   node tests/fwrule_input_test.mjs        (from server/; run_suites.sh runs it when node exists)
//
// 1. Every destination in tests/fw_dest_cases.txt gives what the C store gives (build/fwdest-test
//    checks the same file against fw_parse_dest + fw_format_dest).
// 2. Ports, protocols, actions, the app default, and the precedence order (rules.c "consider").
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { parseDest, parsePort, checkRule, sortByPrecedence, specificity, isDefaultRule, decide, toggleOp, isLan } = await import(path.join(here, '../../sdk/fwrule.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

// 1. the shared destination cases
for (const line of fs.readFileSync(path.join(here, 'fw_dest_cases.txt'), 'utf8').split('\n')) {
  if (!line || line[0] === '#') continue;
  const [input, want] = line.split('\t');
  const d = parseDest(input);
  eq(`dest ${input}`, d.ok ? d.dst : '!', want);
}

// kinds and errors the sheet explains
eq('kind exact', parseDest('example.com').kind, 'domain');
eq('kind wildcard', parseDest('*.example.com').kind, 'wildcard');
eq('kind address', parseDest('192.0.2.7').kind, 'address');
eq('kind network', parseDest('10.0.0.0/8').kind, 'network');
eq('kind v6 network', parseDest('2001:db8::/32').kind, 'network');
eq('err empty', parseDest('  ').error, 'empty');
eq('err wildcard', parseDest('*example.com').error, 'wildcard');
eq('err hostBits', parseDest('10.0.0.1/8').error, 'hostBits');
eq('err prefix', parseDest('10.0.0.0/33').error, 'prefix');
eq('err address', parseDest('256.1.1.1').error, 'address');
eq('err domain', parseDest('exa mple.com').error, 'domain');
eq('trimmed', parseDest('  Example.com  ').dst, 'example.com');

// 2. ports
eq('port empty', parsePort(''), { ok: true, port: '' });
eq('port one', parsePort('443').port, '443');
eq('port range', parsePort('8000-8080').port, '8000-8080');
eq('port range spaces', parsePort('8000 - 8080').port, '8000-8080');
eq('port same', parsePort('53-53').port, '53');
for (const p of ['0', '65536', '080', '9-1', 'abc', '1-', '-1', '1-2-3', '+80']) eq(`port bad ${p}`, parsePort(p).ok, false);

// whole rules
eq('rule ok', checkRule({ dst: '*.Example.com', port: '', proto: '', action: 'deny' }),
  { ok: true, kind: 'wildcard', rule: { dst: '*.example.com', port: '', proto: '', action: 'deny', dir: 'out' } });
eq('rule bad dst', checkRule({ dst: 'x..y', action: 'allow' }).field, 'dst');
eq('rule bad port', checkRule({ dst: 'example.com', port: '70000', action: 'allow' }).field, 'port');
eq('rule bad proto', checkRule({ dst: 'example.com', proto: 'icmp', action: 'allow' }).field, 'proto');
eq('rule bad action', checkRule({ dst: 'example.com', action: 'maybe' }).field, 'action');
eq('rule default refused', checkRule({ dst: 'any', action: 'deny' }).error, 'isDefault');
eq('rule any with port', checkRule({ dst: 'any', port: '25', proto: 'tcp', action: 'deny' }).ok, true);
eq('isDefault', isDefaultRule({ dir: 'out', dst: 'any', port: '', proto: '' }), true);
eq('isDefault port', isDefaultRule({ dir: 'out', dst: 'any', port: '443', proto: '' }), false);

// precedence: exact > wildcard (more labels first) > network (longer prefix) > lan/internet > any;
// then the smaller port range, a concrete protocol, deny before allow, the newest (higher id)
eq('spec order', [specificity({ dst: 'a.example.com' }) > specificity({ dst: '*.a.example.com' }),
  specificity({ dst: '*.a.example.com' }) > specificity({ dst: '*.example.com' }),
  specificity({ dst: '*.com' }) > specificity({ dst: '192.0.2.7' }),
  specificity({ dst: '192.0.2.7' }) > specificity({ dst: '192.0.2.0/24' }),
  specificity({ dst: '0.0.0.0/0' }) > specificity({ dst: 'internet' }),
  specificity({ dst: 'lan' }) > specificity({ dst: 'any' })], [true, true, true, true, true, true]);
const rules = [
  { id: 1, dst: 'any', port: '', proto: '', action: 'deny' },
  { id: 2, dst: '*.example.com', port: '', proto: '', action: 'allow' },
  { id: 3, dst: '*.example.com', port: '443', proto: '', action: 'allow' },
  { id: 4, dst: 'api.example.com', port: '', proto: '', action: 'allow' },
  { id: 5, dst: '10.0.0.0/8', port: '', proto: '', action: 'deny' },
  { id: 6, dst: '10.1.0.0/16', port: '', proto: '', action: 'allow' },
  { id: 7, dst: '*.example.com', port: '443', proto: 'tcp', action: 'allow' },
  { id: 8, dst: '*.example.com', port: '443', proto: 'tcp', action: 'deny' },
  { id: 9, dst: '*.example.com', port: '443', proto: 'tcp', action: 'deny' },
];
eq('sorted', sortByPrecedence(rules).map((r) => r.id), [4, 9, 8, 7, 3, 2, 6, 5, 1]);

// 3. one destination's verdict (the Monitor's per-domain switch): the cases build/fwdest-test runs
//    through fw_decide
const ruleOf = (txt, i) => {
  const [action, dst, a = '', b = ''] = txt.trim().split(/\s+/);
  const proto = a === 'tcp' || a === 'udp' ? a : b;
  const port = a === 'tcp' || a === 'udp' ? '' : a;
  const c = parseDest(dst);
  return { id: i + 1, dir: 'out', dst: c.ok ? c.dst : dst, port, proto, action };
};
let cases = 0;
for (const line of fs.readFileSync(path.join(here, 'fw_domain_cases.txt'), 'utf8').split('\n')) {
  if (!line || line[0] === '#') continue;
  const col = line.split('|').map((x) => x.trim());
  // a rule added again with the same destination, ports and protocol replaces the previous one (rules.c)
  const list = [];
  if (col[0] !== '-') col[0].split(';').forEach((r, i) => {
    const x = ruleOf(r, i);
    const same = list.findIndex((y) => y.dst === x.dst && y.port === x.port && y.proto === x.proto);
    if (same >= 0) list[same] = { ...x, id: list[same].id }; else list.push(x);
  });
  const [proto, port] = col[3].split(/\s+/);
  const names = col[1] === '-' ? [] : col[1].split(',');
  const restricted = col[4] === '-' ? [] : col[4].split(',');
  const d = decide(list, { names, addr: col[2] === '-' ? '' : col[2], proto, port: Number(port) }, restricted);
  eq(`domain case ${++cases} ${col[1]} ${col[2]}`, d ? (d.by === 'restricted' ? 'restricted' : d.action) : 'none', col[5]);
}
eq('lan', [isLan('192.168.1.2'), isLan('100.64.0.1'), isLan('8.8.8.8'), isLan('fe80::1'), isLan('2001:db8::1'), isLan('::1')], [true, true, false, true, false, true]);

// the switch: one click flips the verdict, and going back removes the rule instead of adding another
const q = { name: 'ads.example.com', addr: '93.184.216.34', proto: 'tcp', port: 443 };
eq('toggle: none → add deny', toggleOp([], q), { op: 'add', rule: { dir: 'out', dst: 'ads.example.com', port: '', proto: '', action: 'deny' } });
const one = [{ id: 7, dir: 'out', dst: 'ads.example.com', port: '', proto: '', action: 'deny' }];
eq('toggle: own deny → remove', toggleOp(one, q), { op: 'remove', id: 7 });
const blockAll = [{ id: 1, dir: 'out', dst: 'any', port: '', proto: '', action: 'deny' }];
eq('toggle: blocked by default → add allow', toggleOp(blockAll, q).rule.action, 'allow');
const both = [...blockAll, { id: 2, dir: 'out', dst: 'ads.example.com', port: '', proto: '', action: 'allow' }];
eq('toggle: own allow over deny-all → remove', toggleOp(both, q), { op: 'remove', id: 2 });
const wild = [{ id: 3, dir: 'out', dst: '*.example.com', port: '', proto: '', action: 'deny' }, { id: 4, dir: 'out', dst: 'ads.example.com', port: '', proto: '', action: 'deny' }];
eq('toggle: own deny under a deny wildcard → update to allow', toggleOp(wild, q), { op: 'update', id: 4, rule: { dir: 'out', dst: 'ads.example.com', port: '', proto: '', action: 'allow' } });
eq('toggle: no name → the address', toggleOp([], { addr: '203.0.113.5', proto: 'udp', port: 443 }).rule.dst, '203.0.113.5');
eq('toggle: restricted → nothing', toggleOp([], q, ['no-internet']), null);

// (0.8.5) blocklists and the deny mode, the same order as rules.c (fw_rules_test's cases)
const t = 'tracker.example';
const L = { listed: true };
eq('list: no rules → denied by the list', decide([], { names: [t], addr: '203.0.113.9' }, [], L), { action: 'deny', rule: null, by: 'list' });
eq('list: off → none', decide([], { names: [t], addr: '203.0.113.9' }, [], {}), null);
const broad = [{ id: 1, dir: 'out', dst: 'any', port: '', proto: '', action: 'allow' }, { id: 2, dir: 'out', dst: 'internet', port: '', proto: '', action: 'allow' }];
eq('list: beats any/internet allow rules', decide(broad, { names: [t], addr: '203.0.113.9' }, [], L).by, 'list');
const mine = [...broad, { id: 3, dir: 'out', dst: t, port: '', proto: '', action: 'allow' }];
eq('list: the owner\'s allow for the name wins', decide(mine, { names: [t], addr: '203.0.113.9' }, [], L).rule?.id, 3);
const net = [{ id: 4, dir: 'out', dst: '198.51.100.0/24', port: '', proto: '', action: 'allow' }];
eq('list: an allow for the network wins', decide(net, { names: [], addr: '198.51.100.7' }, [], L).rule?.id, 4);
eq('list: a restriction comes first', decide([], { names: [t], addr: '203.0.113.9' }, ['no-internet'], L).by, 'restricted');
eq('toggle: listed → add an allow for the name', toggleOp([], { name: t, addr: '203.0.113.9', proto: 'tcp', port: 443 }, [], L), { op: 'add', rule: { dir: 'out', dst: t, port: '', proto: '', action: 'allow' } });
const D = { mode: 'deny' };
eq('deny mode: internet', decide([], { addr: '8.8.8.8' }, [], D), { action: 'deny', rule: null, by: 'mode' });
eq('deny mode: LAN', decide([], { addr: '192.168.1.1' }, [], D).action, 'deny');
eq('deny mode: loopback stays', decide([], { addr: '127.0.0.53', port: 53, proto: 'udp' }, [], D), null);
eq('deny mode: ::1 stays', decide([], { addr: '::1' }, [], D), null);
eq('deny mode: a rule allows', decide([{ id: 5, dir: 'out', dst: 'example.org', port: '', proto: '', action: 'allow' }], { names: ['example.org'], addr: '203.0.113.5' }, [], D).action, 'allow');

console.log(`fwrule-input-test: ${n} checks, ${bad} failed`);
process.exit(bad ? 1 : 0);
