// netmodel_test.mjs — Monitor → Network's model (sdk/netmodel.js, 0.8.5): search tokens, filters,
// grouping and sorting. Run by tests/run_suites.sh (needs node).
import { parseSearch, chipFor, addChips, filterFlows, groupFlows, sortTree, fold, normView, parseCidr, cidrContains, matchesSearch } from '../../sdk/netmodel.js';

let n = 0, bad = 0;
const eq = (what, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${what}\n    got  ${g}\n    want ${w}`); }
};

// ---- tokens ----
eq('typed tokens become chips', parseSearch('app:Firefox domain:Example.COM video port:443'), {
  chips: [{ type: 'app', value: 'Firefox' }, { type: 'domain', value: 'example.com' }, { type: 'port', value: '443' }], rest: 'video' });
eq('aliases', parseSearch('host:a.example addr:10.0.0.0/8 protocol:QUIC cc:ar').chips, [
  { type: 'domain', value: 'a.example' }, { type: 'ip', value: '10.0.0.0/8' }, { type: 'proto', value: 'udp' }, { type: 'country', value: 'AR' }]);
eq('unknown type stays text', parseSearch('city:Rosario mail').rest, 'city:Rosario mail');
eq('an IPv6 address is not a "type:"', parseSearch('2001:db8::1').chips, []);
eq('commit: bare words get a type', parseSearch('192.0.2.0/24 53 udp AR cdn.example firefox', true).chips, [
  { type: 'ip', value: '192.0.2.0/24' }, { type: 'port', value: '53' }, { type: 'proto', value: 'udp' }, { type: 'country', value: 'AR' },
  { type: 'domain', value: 'cdn.example' }, { type: 'app', value: 'firefox' }]);
eq('commit: an IPv6 network', chipFor('2001:db8::/32'), { type: 'ip', value: '2001:db8::/32' });
eq('bad values are dropped', [chipFor('port:0'), chipFor('port:70000'), chipFor('ip:300.1.1.1'), chipFor('proto:icmp'), chipFor('nope:x')], [null, null, null, null, null]);
eq('no repeated chips', addChips([{ type: 'app', value: 'a' }], [{ type: 'app', value: 'a' }, { type: 'port', value: '1' }]).length, 2);

// ---- addresses ----
eq('cidr', [cidrContains(parseCidr('10.0.0.0/8'), '10.2.3.4'), cidrContains(parseCidr('10.0.0.0/8'), '11.0.0.1'),
  cidrContains(parseCidr('2001:db8::/32'), '2001:db8:1::5'), cidrContains(parseCidr('2001:db8::/32'), '2001:db9::1'),
  cidrContains(parseCidr('192.0.2.0/24'), '::ffff:192.0.2.9'), cidrContains(parseCidr('192.0.2.7'), '192.0.2.7')], [true, false, true, false, true, true]);
eq('bad cidrs', [parseCidr('10.0.0.0/33'), parseCidr('1.2.3'), parseCidr('::1::2'), parseCidr('a/b/c')], [null, null, null, null]);

// ---- flows ----
const F = (o) => ({ app: 'desktop:firefox', appName: 'Firefox', name: null, addr: '203.0.113.5', port: 443, proto: 'tcp', dir: 'out', rx: 0, tx: 0,
  rateRx: 0, rateTx: 0, cc: 'US', scope: 'internet', conns: 1, denied: 0, last: 0, listed: null, ...o });
const flows = [
  F({ name: 'video.example', addr: '203.0.113.5', port: 443, proto: 'udp', rx: 9000, tx: 500, last: 30 }),
  F({ name: 'video.example', addr: '203.0.113.6', port: 443, proto: 'udp', rx: 1000, tx: 100, last: 20 }),
  F({ name: 'video.example', addr: '203.0.113.6', port: 443, proto: 'tcp', rx: 50, tx: 50, last: 10 }),
  F({ name: 'ads.example', addr: '198.51.100.1', port: 443, rx: 10, tx: 10, denied: 2, listed: 'ads', last: 40 }),
  F({ addr: '192.0.2.44', port: 8080, rx: 5, tx: 5, cc: 'AR' }),
  F({ app: 'lordos:mail', appName: 'Mail', name: 'imap.example', addr: '192.0.2.10', port: 993, rx: 700, tx: 300, cc: 'AR', last: 50 }),
  F({ app: 'lordos:terminal', appName: 'Terminal', name: null, addr: '10.0.0.5', port: 22, dir: 'out', rx: 1, tx: 1, scope: 'lan', cc: '' }),
  F({ app: 'lordos:terminal', appName: 'Terminal', addr: '192.168.1.20', port: 8000, dir: 'in', rx: 3, tx: 3, scope: 'lan', cc: '' }),
];

const tree = sortTree(groupFlows(flows, 'app'), 'most');
eq('apps, busiest first', tree.map((x) => [x.label, x.rx + x.tx]), [['Firefox', 10730], ['Mail', 1000], ['Terminal', 8]]);
const ff = tree[0];
eq('app → destinations', ff.children.map((x) => [x.kind, x.label]), [['domain', 'video.example'], ['domain', 'ads.example'], ['ip', '192.0.2.44']]);
eq('a name with two addresses has an address level', ff.children[0].children.map((x) => [x.kind, x.label]), [['ip', '203.0.113.5'], ['ip', '203.0.113.6']]);
eq('… and ports under each address', ff.children[0].children[1].children.map((x) => x.label), ['UDP 443', 'TCP 443']);
eq('a name with one address goes straight to its ports', ff.children[1].children.map((x) => [x.kind, x.label]), [['port', 'TCP 443']]);
eq('an address row has its ports', ff.children[2].children.map((x) => x.label), ['TCP 8080']);
eq('sums at every level', [ff.children[0].rx, ff.children[0].tx, ff.children[0].conns], [10050, 650, 3]);
eq('denied and listed go up', [ff.denied, ff.listed, ff.children[1].listed], [2, 'ads', 'ads']);

eq('group by domain', sortTree(groupFlows(flows, 'domain'), 'name').map((x) => x.label),
  ['10.0.0.5', '192.0.2.44', '192.168.1.20', 'ads.example', 'imap.example', 'video.example']);
eq('domain → apps', groupFlows(flows, 'domain').find((x) => x.label === 'video.example').children.map((x) => x.label), ['Firefox']);
eq('group by IP', groupFlows(flows, 'ip').length, 7);
eq('group by port', sortTree(groupFlows(flows, 'port'), 'most').map((x) => x.label), ['UDP 443', 'TCP 993', 'TCP 443', 'TCP 8080', 'TCP 8000', 'TCP 22']);
eq('group by country', sortTree(groupFlows(flows, 'country'), 'most').map((x) => x.label), ['US', 'AR', 'lan']);
eq('sort least', sortTree(groupFlows(flows, 'app'), 'least').map((x) => x.label), ['Terminal', 'Mail', 'Firefox']);
eq('sort recent', sortTree(groupFlows(flows, 'app'), 'recent').map((x) => x.label), ['Mail', 'Firefox', 'Terminal']);
eq('sort conns', sortTree(groupFlows(flows, 'app'), 'conns').map((x) => x.label), ['Firefox', 'Terminal', 'Mail']);

// ---- filters ----
const only = (f, q = {}, ctx = {}) => filterFlows(flows, f, q, ctx).length;
eq('direction', [only({ dir: 'in' }), only({ dir: 'out' }), only({})], [1, 7, 8]);
eq('protocol', [only({ proto: 'udp' }), only({ proto: 'tcp' })], [2, 6]);
eq('status', [only({ status: 'denied' }), only({ status: 'allowed' }), only({ status: 'rules' }, {}, { hasRule: (f) => f.app === 'lordos:mail' })], [1, 7, 1]);
eq('search: app chip', only({}, parseSearch('app:mail')), 1);
eq('search: app + domain chips combine', only({}, parseSearch('app:firefox domain:example')), 4);
eq('search: a parent domain covers its names', only({}, parseSearch('domain:example')), 5);
eq('search: two chips of one type: either', only({}, parseSearch('port:22 port:993')), 2);
eq('search: network chip', only({}, parseSearch('ip:192.0.2.0/24')), 2);
eq('search: country by code and by name', [only({}, parseSearch('country:AR')), only({}, parseSearch('country:argen'), { countryName: (cc) => (cc === 'AR' ? 'Argentina' : cc) })], [2, 2]);
eq('search: free text', only({}, { chips: [], rest: 'imap' }), 1);
eq('search: any name of a CNAME chain', matchesSearch(F({ name: 'deb.debian.org', names: ['deb.debian.org', 'debian.map.cdn.example'] }), parseSearch('domain:cdn.example')), true);
eq('search: free text needs every word', only({}, { chips: [], rest: 'firefox 8080' }), 1);
eq('search: nothing', matchesSearch(flows[0], { chips: [{ type: 'port', value: '1' }] }), false);

// ---- fold and the saved view ----
eq('fold', fold([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 8), { list: [1, 2, 3, 4, 5, 6, 7, 8], more: 2 });
eq('fold all', fold([1, 2, 3], 2, true).more, 0);
eq('view defaults', normView(null), { group: 'app', dir: 'all', proto: 'all', status: 'all', sort: 'most' });
eq('view keeps only known values', normView({ group: 'port', dir: 'sideways', sort: 'least', x: 1 }), { group: 'port', dir: 'all', proto: 'all', status: 'all', sort: 'least' });

console.log(`netmodel-test: ${n} checks, ${bad} failed`);
process.exit(bad ? 1 : 0);
