// remotedesk_test.mjs — Remote Desk's saved computers (sdk/remotedeskmodel.js): the migration of what older versions
// kept (a PC name in "Computer" with its IP in "Domain"), the address field, the user name, sorting and search.
import assert from 'node:assert';
import { migrate, parseAddress, splitUser, sortDevices, filterDevices, groupDevices, looksLikeAddress, userOk, swapButtons, thumbPath, duplicate, titleOf } from '../../sdk/remotedeskmodel.js';

// the case reported from the Pi: the name in Host, the IP in Domain
{
  const { value, changed } = migrate({ devices: [{ proto: 'rdp', host: 'officepc', port: 0, user: 'iperfex', domain: '172.17.0.6', name: '', res: 'auto', full: false, folders: [] }], certs: { 'x:0': 'AA' } });
  assert.equal(changed, true);
  assert.equal(value.v, 2);
  const [d] = value.devices;
  assert.equal(d.host, '172.17.0.6');
  assert.equal(d.name, 'officepc');
  assert.equal(d.port, 0);
  assert.equal(d.legacy, 'officepc:0');          // the remembered password is still found
  assert.equal(d.user, undefined);
  assert.equal(d.domain, undefined);
  assert.deepEqual(value.creds, [{ id: d.cred, user: 'iperfex', domain: '' }]);
  assert.deepEqual(value.certs, { 'x:0': 'AA' });
  // done once: the result is kept as it is
  assert.deepEqual(migrate(value), { value, changed: false });
}
// the same computer saved right afterwards (host 172.17.0.6, name officepc, no domain): left as it is
{
  const right = { proto: 'rdp', host: '172.17.0.6', port: 0, user: 'iperfex', domain: '', name: 'officepc' };
  const { value } = migrate({ devices: [right] });
  const [d] = value.devices;
  assert.equal(d.host, '172.17.0.6');
  assert.equal(d.name, 'officepc');
  assert.equal(d.legacy, '172.17.0.6:0');
  assert.deepEqual(value.creds.map((c) => [c.user, c.domain]), [['iperfex', '']]);
  // both saved (the newest first): one computer, one credential
  const both = migrate({ devices: [right, { proto: 'rdp', host: 'officepc', port: 0, user: 'iperfex', domain: '172.17.0.6', name: '' }] }).value;
  assert.equal(both.devices.length, 1);
  assert.equal(both.devices[0].legacy, '172.17.0.6:0');
  assert.equal(both.creds.length, 1);
}
// a real directory domain stays a domain
{
  const { value } = migrate({ devices: [{ proto: 'rdp', host: 'pc7', port: 0, user: 'ana', domain: 'CORP' },
    { proto: 'rdp', host: 'pc8', port: 3390, user: 'ana', domain: 'corp.example.com' },
    { proto: 'rdp', host: 'pc9.corp.example.com', port: 0, user: 'bob', domain: '10.0.0.1' }] });
  const [a, b, c] = value.devices;
  assert.equal(a.host, 'pc7');
  assert.equal(b.host, 'pc8');                    // a dotted domain that isn't this PC's own name
  assert.equal(c.host, 'pc9.corp.example.com');   // the host was already an address
  assert.deepEqual(value.creds.find((x) => x.id === a.cred), { id: a.cred, user: 'ana', domain: 'CORP' });
  assert.equal(value.creds.find((x) => x.id === b.cred).domain, 'corp.example.com');
  assert.equal(value.creds.find((x) => x.id === c.cred).domain, '10.0.0.1');
  assert.notEqual(a.cred, b.cred);
}
// the host's own dotted name in Domain is the address too
{
  const { value } = migrate({ devices: [{ proto: 'rdp', host: 'office', port: 0, user: '', domain: 'office.lan', name: 'Office' }] });
  assert.equal(value.devices[0].host, 'office.lan');
  assert.equal(value.devices[0].name, 'Office');
  assert.equal(value.devices[0].cred, '');
  assert.equal(value.creds.length, 0);
}
// the oldest format (one list per protocol) and nothing at all
{
  const { value } = migrate({ rdp: [{ host: 'a.lan', port: 0, user: 'u', domain: '' }], vnc: [{ host: '10.0.0.9', port: 5901 }] });
  assert.deepEqual(value.devices.map((d) => [d.proto, d.host, d.port]), [['rdp', 'a.lan', 0], ['vnc', '10.0.0.9', 5901]]);
  assert.equal(value.rdp, undefined);
  assert.notEqual(value.devices[0].id, value.devices[1].id);
  assert.deepEqual(migrate(null).value.devices, []);
  assert.deepEqual(migrate({ devices: [null, { host: '' }, 7] }).value.devices, []);
}

// the address field
assert.deepEqual(parseAddress(' 172.17.0.6 '), { host: '172.17.0.6', port: 0 });
assert.deepEqual(parseAddress('pc.local:3390'), { host: 'pc.local', port: 3390 });
assert.deepEqual(parseAddress('officepc'), { host: 'officepc', port: 0 });
assert.equal(parseAddress(''), null);
assert.equal(parseAddress('pc:99999'), null);
assert.equal(parseAddress('my pc'), null);
assert.equal(parseAddress('-x'), null);
assert.equal(looksLikeAddress('officepc'), false);
assert.equal(looksLikeAddress('172.17.0.6'), true);
assert.equal(looksLikeAddress('pc.local'), true);
assert.equal(looksLikeAddress('localhost'), true);
assert.equal(looksLikeAddress('300.1.1.1'), false);

// the user name
assert.deepEqual(splitUser('CORP\\ana'), { user: 'ana', domain: 'CORP' });
assert.deepEqual(splitUser('ana@example.com'), { user: 'ana@example.com', domain: '' });
assert.deepEqual(splitUser('ana', 'CORP'), { user: 'ana', domain: 'CORP' });
assert.deepEqual(splitUser(' iperfex '), { user: 'iperfex', domain: '' });
assert.deepEqual(splitUser('\\ana'), { user: '\\ana', domain: '' });
assert.equal(userOk('ana@example.com'), true);
assert.equal(userOk('a b'), false);
assert.equal(userOk('@x'), false);

// sorting, search, groups
{
  const list = [{ id: '1', host: 'b.lan', name: '', proto: 'rdp', last: 5, group: '', cred: 'c1' },
    { id: '2', host: '10.0.0.2', name: 'Alpha', proto: 'vnc', last: 9, group: 'Lab', fav: true, cred: '' },
    { id: '3', host: '10.0.0.3', name: 'charlie', proto: 'rdp', last: 0, group: '', cred: '' }];
  assert.deepEqual(sortDevices(list, 'name').map((d) => d.id), ['2', '1', '3']);
  assert.deepEqual(sortDevices(list, 'last').map((d) => d.id), ['2', '1', '3']);
  assert.deepEqual(filterDevices(list, { kind: 'rdp' }).map((d) => d.id), ['1', '3']);
  assert.deepEqual(filterDevices(list, { favorites: true }).map((d) => d.id), ['2']);
  assert.deepEqual(filterDevices(list, { query: 'CHAR' }).map((d) => d.id), ['3']);
  assert.deepEqual(filterDevices(list, { query: 'iperfex', creds: [{ id: 'c1', user: 'iperfex', domain: '' }] }).map((d) => d.id), ['1']);
  assert.deepEqual(filterDevices(list, { query: 'lab' }).map((d) => d.id), ['2']);
  assert.deepEqual(groupDevices(list, ['Lab', 'Empty']).map(([g, l]) => [g, l.length]), [['', 2], ['Empty', 0], ['Lab', 1]]);
  const copy = duplicate(list[1], list, (n) => `${n} copy`);
  assert.notEqual(copy.id, '2');
  assert.equal(copy.name, 'Alpha copy');
  assert.equal(copy.shot, 0);
  assert.equal(titleOf(list[0]), 'b.lan');
}

// the pointer and the picture
assert.equal(swapButtons(1, true), 4);
assert.equal(swapButtons(4, true), 1);
assert.equal(swapButtons(2 | 8, true), 10);
assert.equal(swapButtons(1, false), 1);
assert.equal(thumbPath('/home/a', { id: 'd1', shot: 0 }), '');
assert.equal(thumbPath('/home/a', { id: 'd1', shot: 17 }), '/home/a/.local/share/lordos/remote-desk/thumbs/d1.png#17');
assert.equal(thumbPath('/home/a', { id: '../x', shot: 17 }), '');
console.log('remotedesk-test: ok');

// .rdp files
{
  const { parseRdp, exportRdp, decodeRdp, resSize, fileNameOf } = await import('../../sdk/remotedeskmodel.js');
  const text = 'screen mode id:i:2\r\ndesktopwidth:i:1920\r\ndesktopheight:i:1080\r\nsession bpp:i:16\r\nfull address:s:172.17.0.6:3390\r\naudiomode:i:1\r\nusername:s:CORP\\ana\r\nadministrative session:i:1\r\nredirectclipboard:i:1\r\n';
  const r = parseRdp(text);
  assert.equal(r.device.host, '172.17.0.6');
  assert.equal(r.device.port, 3390);
  assert.equal(r.device.full, true);
  assert.equal(r.device.res, '1920x1080');
  assert.equal(r.device.depth, 16);
  assert.equal(r.device.sound, 'there');
  assert.equal(r.device.admin, true);
  assert.deepEqual([r.user, r.domain], ['ana', 'CORP']);
  assert.equal(parseRdp('username:s:x\n'), null);
  assert.equal(parseRdp('full address:s:pc.lan\naudiomode:i:2\ndesktopwidth:i:1366\ndesktopheight:i:768\n').device.res, '1366x768');
  assert.equal(parseRdp('full address:s:pc.lan\naudiomode:i:2\n').device.sound, 'none');
  // UTF-16 with its byte order mark, as such files are usually saved
  const u16 = [0xff, 0xfe, ...[...'full address:s:pc.lan\r\n'].flatMap((c) => [c.charCodeAt(0), 0])];
  assert.equal(parseRdp(decodeRdp(new Uint8Array(u16))).device.host, 'pc.lan');
  assert.equal(decodeRdp(new Uint8Array([0xef, 0xbb, 0xbf, 0x61])), 'a');
  // and back: no password ever
  const out = exportRdp({ ...r.device, name: 'Office' }, { user: 'ana', domain: 'CORP' });
  assert.match(out, /full address:s:172\.17\.0\.6:3390/);
  assert.match(out, /username:s:CORP\\ana/);
  assert.match(out, /audiomode:i:1/);
  assert.doesNotMatch(out, /password/i);
  const again = parseRdp(out);
  assert.deepEqual([again.device.host, again.device.port, again.device.res, again.device.full, again.device.depth], ['172.17.0.6', 3390, '1920x1080', true, 16]);
  assert.equal(fileNameOf({ name: 'My/PC', host: 'x' }), 'My_PC.rdp');
  assert.deepEqual(resSize('1366x768'), [1366, 768]);
  assert.deepEqual(resSize('auto'), [0, 0]);
  assert.deepEqual(resSize('100x100'), [0, 0]);
  console.log('remotedesk-test: rdp files ok');
}
