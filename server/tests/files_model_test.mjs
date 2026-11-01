// Files without a screen: what an address means and how columns move. node server/tests/files_model_test.mjs
import { parseAddress, sshLine, remoteUri, hostAddress } from '../../apps/files/connect.js';
import { normalColumns, visible, toggleColumn, moveColumn, resize, dropBefore, totalWidth, MIN_W, MAX_W } from '../../apps/files/columns.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };
const p = (s) => parseAddress(s);
eq('default smb', [p('192.168.1.5').scheme, p('192.168.1.5').kind, p('192.168.1.5').share], ['smb', 'mount', '']);
eq('share', p('smb://nas/Public/sub').share, 'Public');
eq('nfs path', p('nfs://10.0.0.1/srv/data').path, '/srv/data');
eq('vnc', [p('vnc://127.0.0.1:5969').kind, p('vnc://127.0.0.1:5969').port], ['remote', 5969]);
eq('rdp uri', remoteUri(p('rdp://ana@host:3390')), 'rdp://ana@host:3390');
eq('ssh line', sshLine(p('ssh://ana@host:2222')), 'ssh -p 2222 ana@host');
eq('sftp line', sshLine(p('sftp://host')), 'sftp host');
eq('ftp folder', p('ftp://host').kind, 'folder');
eq('ssh folder', p('ssh://ana@h').kind, 'folder');
eq('web', p('https://host/a').kind, 'web');
for (const bad of ['ssh://-oProxyCommand=x', 'ssh://a;rm@host', 'vnc://$(x)', 'smb://a b', 'nfs://h/../etc', 'ssh://host:99999', 'ssh://-x@host', 'smb://h/a\\b', 'x`y', 'smb://h/a|b']) eq(`refused ${bad}`, !!p(bad).error, true);
eq('unknown scheme', p('gopher://h').error, 'scheme');
eq('empty', p('  ').error, 'empty');
const hosts = [{ addr: '10.0.0.2', name: 'NAS', services: [] }, { addr: '10.0.0.3', name: '', services: [{ name: 'Office' }] }];
eq('names', [hostAddress(hosts, 'nas.local'), hostAddress(hosts, 'office'), hostAddress(hosts, 'x'), hostAddress(hosts, '10.1.1.1')], ['10.0.0.2', '10.0.0.3', null, '10.1.1.1']);

let c = normalColumns(null);
eq('default', visible(c), ['date', 'size', 'kind']);
c = toggleColumn(c, 'owner');
eq('added', visible(c), ['date', 'size', 'kind', 'owner']);
c = moveColumn(c, 'kind', 'date');
eq('moved', visible(c), ['kind', 'date', 'size', 'owner']);
c = moveColumn(c, 'kind', null);
eq('to the end', visible(c).pop(), 'kind');
eq('clamped', [resize(c, 'size', 1).width.size, resize(c, 'size', 1e6).width.size], [MIN_W, MAX_W]);
eq('stored junk', visible(normalColumns({ order: ['x', 'size', 'size'], shown: ['nope', 'size'], width: { size: 'a' } })), ['size']);
const d = normalColumns(null);
eq('drop', [dropBefore(d, 10), dropBefore(d, 200), dropBefore(d, 900)], ['date', 'kind', null]);
eq('total', totalWidth(d), 350);
console.log(`files-model-test: ${ok} ok`);
