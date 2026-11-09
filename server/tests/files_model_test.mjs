// Files without a screen: what an address means and how columns move. node server/tests/files_model_test.mjs
import { clampSide, SIDE_MIN, SIDE_MAX, viewFor, viewKey, folderViewsWith, clampZoom, zoomWith, zoomOf, iconMetrics, listMetrics, iconColumns, ZOOM_MIN, ZOOM_MAX } from '../../apps/files/viewprefs.js';
import { parseAddress, sshLine, remoteUri, hostAddress, looksDav, davOf } from '../../apps/files/connect.js';
import { offersWay, foundHosts, connectedEntries, shareKind, netGroups, withHostName, shareParts } from '../../apps/files/netplaces.js';
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

// #59: an http(s) address names WebDAV by its path, or is asked about; as a folder it is webdav:// or davs://
eq('dav paths', ['/remote.php/dav/files/ana', '/remote.php/webdav', '/dav', '/webdav/x', '/DAV', '/', '/davide', '/docs'].map(looksDav), [true, true, true, true, true, false, false, false]);
eq('davs of https', davOf(p('https://ana@cloud.example.org/remote.php/dav/')), { scheme: 'davs', kind: 'folder', host: 'cloud.example.org', port: 0, user: 'ana', path: '/remote.php/dav', text: 'davs://ana@cloud.example.org/remote.php/dav' });
eq('webdav of http with a port', davOf(p('http://nas:8080/dav')).text, 'webdav://nas:8080/dav');
eq('standard port dropped', davOf(p('http://nas:80/dav')).port, 0);
eq('davs again', p(davOf(p('https://nas/dav')).text).kind, 'folder');

// #59: Network lists what has a way in, and the places this person is connected to first
const repeater = { addr: '192.168.1.2', name: 'repeater', smb: false, nfs: false, shares: [], exports: [], services: [{ kind: 'http', port: 80 }] };
const printer = { addr: '192.168.1.3', name: '', shares: [], exports: [], services: [{ kind: 'printer', port: 631 }] };
const photo = { addr: '192.168.1.4', name: 'PC-PHOTO', smb: true, shares: [], exports: [], services: [] };
const mac = { addr: '192.168.1.5', name: '', shares: [], exports: [], services: [{ kind: 'afp', port: 548 }] };
const dav = { addr: '192.168.1.6', name: '', shares: [], exports: [], services: [{ kind: 'webdavs', port: 443 }] };
const screen = { addr: '192.168.1.7', name: '', shares: [], exports: [], services: [{ kind: 'screen', port: 5900 }] };
eq('ways in', [repeater, printer, photo, mac, dav, screen, null].map(offersWay), [false, false, true, true, true, true, false]);
const fh = foundHosts([repeater, printer, photo, { addr: '' }], false);
eq('found', [fh.shown.map((h) => h.addr), fh.hidden], [['192.168.1.4'], 2]);
eq('every device', foundHosts([repeater, printer, photo], true).shown.length, 3);
eq('no hosts', foundHosts(undefined).hidden, 0);
const ce = connectedEntries({
  home: '/home/ana',
  volumes: [{ path: '/media/ana/Public on 192.168.1.4', name: 'Public on 192.168.1.4', fs: 'cifs' }, { path: '/media/ana/USB', name: 'USB', fs: 'vfat' },
    { path: '/home/ana/Cloud/Drive', name: 'Drive', fs: 'fuse.rclone' }, { path: '/home/ana/.local/share/lordos/remote/davs on nas', name: 'davs on nas', fs: 'fuse.rclone' }],
  pins: [{ address: 'smb://192.168.1.4/Public', path: '/media/ana/Public on 192.168.1.4', name: 'Public on 192.168.1.4' }, { address: 'smb://192.168.1.9/Backup', path: '/media/ana/Backup on 192.168.1.9', name: 'Backup on 192.168.1.9' }],
  accounts: [{ id: 'g1', name: 'Drive', mount: '/home/ana/Cloud/Drive', state: 'idle' }, { id: 'h', name: 'Hidden', mount: '/home/ana/Cloud/H' }, { id: 'n', name: 'No mount' }],
  hiddenAccounts: ['h'],
});
eq('connected kinds', ce.map((e) => [e.netPlace.kind, e.netPlace.path]), [['share', '/media/ana/Public on 192.168.1.4'], ['share', '/home/ana/.local/share/lordos/remote/davs on nas'],
  ['pin', '/media/ana/Backup on 192.168.1.9'], ['cloud', '/home/ana/Cloud/Drive']]);
eq('no file path reaches a mount point', ce.every((e) => e.path.startsWith('place:') && !e.dir), true);
eq('nothing given', connectedEntries({ accounts: null, pins: null, volumes: null }), []);
eq('share kinds', [shareKind('x on 1.2.3.4', 'cifs'), shareKind('e on h', 'nfs4'), shareKind('davs on nas', 'fuse.rclone'), shareKind('sftp on h:2222', 'fuse.rclone'), shareKind('odd', 'fuse.rclone')], ['smb', 'nfs', 'davs', 'sftp', 'remote']);
const tt = (k) => k;
eq('groups', netGroups([{ path: 'net:1' }, { path: 'place:/a', netPlace: {} }], tt).map((g) => [g.label, g.items.map((e) => e.path)]), [['files.net.group.connected', ['place:/a']], ['files.net.group.found', ['net:1']]]);
eq('one group untitled', netGroups([{ path: 'net:1' }], tt).map((g) => g.label), ['']);
// GitHub #88: a share on 192.168.1.25 shows as the server's name once Files found or was given one
let names = withHostName({}, '192.168.1.25', 'NAS-SALON');
eq('share: the name kept', shareParts('software on 192.168.1.25', names), { share: 'software', host: 'NAS-SALON' });
eq('share: no name known, the address', shareParts('software on 10.0.0.9', names), { share: 'software', host: '10.0.0.9' });
eq('share: not a share folder', shareParts('Backup', names), null);
eq('share: the address itself is not a name', withHostName(names, '10.0.0.9', '10.0.0.9'), names);
eq('share: control characters refused', withHostName(names, '10.0.0.9', 'a\u0007b'), names);
eq('share: a host named like an Object key', shareParts('x on constructor', {}), { share: 'x', host: 'constructor' });
for (let i = 0; i < 70; i++) names = withHostName(names, `10.0.1.${i}`, `pc${i}`);
eq('share: at most 64 names, the oldest go', [Object.keys(names).length, names['192.168.1.25'], names['10.0.1.69']], [64, undefined, 'pc69']);
names = withHostName(names, '10.0.1.10', 'renamed');
eq('share: a new name for an address replaces the old one', [names['10.0.1.10'], Object.keys(names).pop()], ['renamed', '10.0.1.10']);
console.log(`files-model-test: ${ok} ok`);
