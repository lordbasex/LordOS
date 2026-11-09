// login_model_test.mjs — sign-in forms and Passwords (sdk/loginmodel.js): the 🔑 of a server reached by its address
// offers only entries for exactly that host (no title guesses), "Save to Passwords?" is not offered for a login kept
// already or a password that was filled, and what a sign-in is saved as.
import { hostOf, loginHosts, shareHosts, hostMatches, exactMatches, alreadySaved, typedSecret, loginEntry, LOGIN_KINDS } from '../../sdk/loginmodel.js';

let n = 0, bad = 0;
const eq = (what, got, want) => { n++; if (JSON.stringify(got) !== JSON.stringify(want)) { bad++; console.log(`✗ ${what}\n    got  ${JSON.stringify(got)}\n    want ${JSON.stringify(want)}`); } };

// hosts
eq('plain host', hostOf('NAS.local'), 'nas.local');
eq('scheme, user, port, path', hostOf('smb://ana@NAS.local:445/share'), 'nas.local');
eq('www dropped', hostOf('https://www.example.org/login'), 'example.org');
eq('IPv4 with port', hostOf('192.168.1.10:22'), '192.168.1.10');
eq('IPv6 in brackets', hostOf('sftp://[fe80::1]:22/'), 'fe80::1');
eq('bare IPv6 keeps its colons', hostOf('fe80::1'), 'fe80::1');
eq('empty', [hostOf(''), hostOf(null), hostOf('   ')], ['', '', '']);
eq('a name a machine announces is not a host', [hostOf("Ana's laptop"), hostOf('Living room TV'), hostOf('nas-01')], ['', '', 'nas-01']);
eq('distinct non-empty hosts', loginHosts('192.168.1.10', 'NAS', '', null, ['nas', '192.168.1.10:445']), ['192.168.1.10', 'nas']);

// matching
const entries = [
  { id: 'a', cat: 'network', title: 'NAS at home', sub: 'ana', host: '192.168.1.10' },
  { id: 'b', cat: 'internet', title: 'Router', sub: 'admin', url: 'http://192.168.1.1/' },
  { id: 'c', cat: 'ssh', title: 'nas backup', sub: 'root', host: 'nas.local' },
  { id: 'd', cat: 'internet', title: '192.168.1.10 notes', sub: 'x' },                 // the host only in its title
  { id: 'e', cat: 'network', title: 'Old NAS', sub: 'ana', host: '192.168.1.10', trash: 1700000000 },
  { id: 'f', cat: 'network', title: 'Other', sub: 'bob', host: '192.168.1.100' },       // a longer address with the same start
  { id: 'g', cat: 'internet', title: 'sub', sub: 'z', url: 'https://files.nas.local' },  // a subdomain of the host
];
eq('exact host field', hostMatches(entries[0], '192.168.1.10'), true);
eq('site host', hostMatches(entries[1], '192.168.1.1'), true);
eq('no title guess', hostMatches(entries[3], '192.168.1.10'), false);
eq('no prefix match', hostMatches(entries[5], '192.168.1.10'), false);
eq('no subdomain match', hostMatches(entries[6], 'nas.local'), false);
eq('nothing for an empty host', hostMatches(entries[0], ''), false);
eq('entries for the address or the name it was found under, trash left out',
  exactMatches(entries, ['192.168.1.10', 'NAS.local']).map((e) => e.id), ['a', 'c']);
eq('each listed with its user name and host', exactMatches(entries, ['nas.local'])[0], { id: 'c', title: 'nas backup', username: 'root', host: 'nas.local' });
eq('none without a host', exactMatches(entries, []), []);

// a shared folder: only the address the password goes to, never the name the machine announces
eq('share: the address only', shareHosts({ addr: '192.168.1.10', host: 'nas.local' }), ['192.168.1.10']);
eq('spoofing: a device at another address announcing a kept server\'s name is offered nothing',
  exactMatches(entries, shareHosts({ addr: '192.168.1.66', host: 'nas.local' })), []);
eq('spoofing: announcing a kept server\'s address as its name changes nothing',
  exactMatches(entries, shareHosts({ addr: '192.168.1.66', host: '192.168.1.10' })), []);
eq('spoofing: a typed name resolved from announcements to another address is offered nothing',
  exactMatches(entries, shareHosts({ addr: '192.168.1.66', host: 'nas.local', user: 'ana' })), []);
eq('the real server, at its address', exactMatches(entries, shareHosts({ addr: '192.168.1.10', host: 'whatever' })).map((e) => e.id), ['a']);
eq('no address, nothing', shareHosts({ host: 'nas.local' }), []);
eq('kept already is checked against the address too', alreadySaved(entries, shareHosts({ addr: '192.168.1.66', host: '192.168.1.10' }), 'ana'), false);

// already saved
eq('kept for that host and user', alreadySaved(entries, ['192.168.1.10'], 'ana'), true);
eq('domain and case ignored', alreadySaved(entries, ['192.168.1.10'], 'WORKGROUP\\Ana'), true);
eq('another user on that host: not kept', alreadySaved(entries, ['192.168.1.10'], 'bob'), false);
eq('no user given: any login for the host', alreadySaved(entries, ['192.168.1.1'], ''), true);
eq('another host', alreadySaved(entries, ['10.0.0.5'], 'ana'), false);

// typed or filled
eq('typed', typedSecret('s3cret', ''), true);
eq('filled from Passwords or the keyring, unchanged', typedSecret('s3cret', 's3cret'), false);
eq('filled then changed', typedSecret('s3cret!', 's3cret'), true);
eq('empty', typedSecret('', ''), false);

// the entry a sign-in is saved as
eq('SMB share', loginEntry({ kind: 'smb', host: '192.168.1.10', user: 'WORKGROUP\\ana', password: 'pw' }), {
  cat: 'network', title: 'SMB WORKGROUP\\ana@192.168.1.10', tags: [],
  f: [{ k: 'device', t: 'text', v: 'SMB' }, { k: 'host', t: 'text', v: '192.168.1.10' }, { k: 'username', t: 'text', v: 'WORKGROUP\\ana' }, { k: 'password', t: 'hidden', v: 'pw' }],
});
eq('sftp is an ssh login, with its port', loginEntry({ kind: 'sftp', host: 'nas.local', port: 2222, user: 'ana', password: 'pw' }), {
  cat: 'ssh', title: 'SFTP ana@nas.local', tags: [],
  f: [{ k: 'host', t: 'text', v: 'nas.local' }, { k: 'port', t: 'number', v: '2222' }, { k: 'username', t: 'text', v: 'ana' }, { k: 'password', t: 'hidden', v: 'pw' }],
});
eq('VNC without a user', loginEntry({ kind: 'vnc', host: 'pi.local', port: 5900, password: 'pw' }).title, 'VNC pi.local');
eq('a port of 0 or out of range is left out', [loginEntry({ kind: 'rdp', host: 'h', port: 0, user: 'u', password: 'p' }), loginEntry({ kind: 'rdp', host: 'h', port: 70000, user: 'u', password: 'p' })]
  .map((e) => e.f.some((f) => f.k === 'port')), [false, false]);
eq('nothing without a password, a host or a known kind', [loginEntry({ kind: 'smb', host: 'h', password: '' }), loginEntry({ kind: 'smb', host: '', password: 'p' }),
  loginEntry({ kind: 'telnet', host: 'h', password: 'p' }), loginEntry()], [null, null, null, null]);
eq('the saved entry is found again by its host and user', alreadySaved([{ id: 'n', title: 'x', sub: 'ana', host: loginEntry({ kind: 'ftp', host: 'ftp.local', user: 'ana', password: 'p' }).f.find((f) => f.k === 'host').v }],
  ['ftp.local'], 'ana'), true);
eq('every kind Files and Remote Desk sign in with', LOGIN_KINDS, ['smb', 'sftp', 'ftp', 'ftps', 'webdav', 'davs', 'rdp', 'vnc']);

console.log(`login_model_test: ${n - bad}/${n} checks`);
process.exit(bad ? 1 : 0);
