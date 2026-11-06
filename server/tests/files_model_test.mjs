// Files without a screen: what an address means and how columns move. node server/tests/files_model_test.mjs
import { clampSide, SIDE_MIN, SIDE_MAX, viewFor, viewKey, folderViewsWith, clampZoom, zoomWith, zoomOf, iconMetrics, listMetrics, iconColumns, ZOOM_MIN, ZOOM_MAX } from '../../apps/files/viewprefs.js';
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
// each place keeps its view and sort; a place with none takes the default; odd values don't count
{
  const eq = (a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error(`${JSON.stringify(a)} != ${JSON.stringify(b)}`); };
  const l = { view: 'list', sort: { by: 'date', desc: true } };
  const p = { folderViews: folderViewsWith({}, '/a', l), defaultView: { view: 'columns', sort: { by: 'name', desc: false } } };
  eq(viewFor(p, { type: 'dir', path: '/a' }), { ...l, zoom: {} });
  eq(viewFor(p, { type: 'dir', path: '/b' }).view, 'columns');
  eq(viewFor({ folderViews: {}, defaultView: null }, { type: 'dir', path: '/b' }).view, 'icons');
  eq(viewFor({ folderViews: { '/a': { view: 'evil', sort: {} } }, defaultView: null }, { type: 'dir', path: '/a' }).view, 'icons');
  eq(viewKey({ type: 'network' }), ':network'); eq(viewKey({ type: 'search' }), null);
  let m = {};
  for (let i = 0; i < 405; i++) m = folderViewsWith(m, `/f${i}`, l);
  eq(Object.keys(m).length, 400); eq(!!m['/f0'], false);
  eq([clampSide(10), clampSide(9999), clampSide(220), clampSide('x')], [SIDE_MIN, SIDE_MAX, 220, 200]);
}
// zoom (issue #33): steps per place and per view, kept with the view and the sort; odd values don't count
{
  eq('clamp', [clampZoom(-9), clampZoom(9), clampZoom(1), clampZoom(1.5), clampZoom('2'), clampZoom(undefined)], [ZOOM_MIN, ZOOM_MAX, 1, 0, 0, 0]);
  eq('usual sizes', [iconMetrics(0).icon, iconMetrics(0).cell, listMetrics(0).icon, listMetrics(0).row, listMetrics(0).font], [64, 100, 16, 24, 13]);
  const grows = (f, k) => { for (let z = ZOOM_MIN; z < ZOOM_MAX; z++) if (!(f(z + 1)[k] > f(z)[k])) return false; return true; };
  eq('icons grow', [grows(iconMetrics, 'icon'), grows(iconMetrics, 'cell'), grows(iconMetrics, 'font')], [true, true, true]);
  eq('list grows', [grows(listMetrics, 'icon'), grows(listMetrics, 'row'), grows(listMetrics, 'font')], [true, true, true]);
  eq('row fits the icon', [...Array(ZOOM_MAX - ZOOM_MIN + 1)].every((_, i) => listMetrics(ZOOM_MIN + i).row >= listMetrics(ZOOM_MIN + i).icon + 6), true);
  eq('columns', [iconColumns(724, 0), iconColumns(724, 4), iconColumns(724, ZOOM_MIN), iconColumns(50, 4)], [6, 3, 8, 1]);
  let z = zoomWith({}, 'icons', 2);
  z = zoomWith(z, 'list', -1);
  eq('per view', [zoomOf(z, 'icons'), zoomOf(z, 'list'), zoomOf(z, 'gallery'), zoomOf(null, 'icons')], [2, -1, 0, 0]);
  eq('back to 0 drops it', zoomWith(z, 'icons', 0), { list: -1 });
  eq('clamped on the way in', zoomWith({}, 'icons', 99), { icons: ZOOM_MAX });
  const pz = { folderViews: folderViewsWith({}, '/p', { view: 'icons', sort: { by: 'name' }, zoom: z }), defaultView: null };
  eq('kept with the folder', viewFor(pz, { type: 'dir', path: '/p' }).zoom, { icons: 2, list: -1 });
  eq('other folders usual', viewFor(pz, { type: 'dir', path: '/q' }).zoom, {});
  const junk = { folderViews: { '/j': { view: 'list', sort: { by: 'name' }, zoom: { icons: 40, list: 'x', evil: 3 } } }, defaultView: null };
  eq('junk zoom', viewFor(junk, { type: 'dir', path: '/j' }).zoom, {});
  eq('no zoom stored when usual', 'zoom' in folderViewsWith({}, '/u', { view: 'list', sort: { by: 'name' }, zoom: {} })['/u'], false);
}
console.log(`files-model-test: ${ok} ok`);
