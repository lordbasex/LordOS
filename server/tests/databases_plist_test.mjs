// Databases: the XML plist reader and the Sequel Ace / Sequel Pro favorites import, on synthetic files (never a real
// Favorites.plist). node server/tests/databases_plist_test.mjs
import { parsePlist, unescapeXml, importSequel, mergeImport, favoriteFrom, candidatePaths, SOURCES } from '../../apps/databases/plist.js';
import { validateFavorite, cleanFavorite } from '../../apps/databases/model.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}:\n  got  ${JSON.stringify(a)}\n  want ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };
const throws = (name, fn, code) => { let got = null; try { fn(); } catch (e) { got = e.code || e.message; } eq(name, got, code); };

const HEAD = '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0">\n';
const plist = (body) => `${HEAD}${body}\n</plist>\n`;

// ---- the parser ----
eq('scalars', parsePlist(plist(`<dict>
  <key>s</key><string>a &amp; b &lt;c&gt; &#233;&#x1F600;</string>
  <key>i</key><integer>-42</integer>
  <key>big</key><integer>123456789012345678901</integer>
  <key>r</key><real>3.5</real>
  <key>t</key><true/>
  <key>f</key><false/>
  <key>d</key><date>2026-11-08T21:14:05Z</date>
  <key>b</key><data>
    SGVs
    bG8=
  </data>
  <key>e</key><string/>
  <key>e2</key><string></string>
  <key>sp</key><string>  two  spaces  </string>
  <key>ed</key><dict/>
  <key>ea</key><array/>
</dict>`)), { s: 'a & b <c> é😀', i: -42, big: '123456789012345678901', r: 3.5, t: true, f: false, d: '2026-11-08T21:14:05Z', b: { data: 'SGVsbG8=' }, e: '', e2: '', sp: '  two  spaces  ', ed: {}, ea: [] });
eq('nested', parsePlist(plist('<array><dict><key>a</key><array><integer>1</integer><string>x</string></array></dict><!-- note --><true/></array>')), [{ a: [1, 'x'] }, true]);
eq('cdata', parsePlist(plist('<string><![CDATA[<raw> & stuff]]></string>')), '<raw> & stuff');
eq('no plist wrapper', parsePlist('<dict><key>a</key><integer>1</integer></dict>'), { a: 1 });
eq('proto key ignored', Object.keys(parsePlist(plist('<dict><key>__proto__</key><dict><key>x</key><true/></dict></dict>'))), []);
eq('unescape', unescapeXml('&quot;&apos;&unknown;&#99999999;'), `"'&unknown;&#99999999;`);
throws('binary plist', () => parsePlist('bplist00\u0000\u0001'), 'binary');
throws('broken', () => parsePlist(plist('<dict><key>a</key>')), 'syntax');
throws('bad integer', () => parsePlist(plist('<integer>12x</integer>')), 'syntax');
throws('bad real', () => parsePlist(plist('<real>abc</real>')), 'syntax');
throws('unknown tag', () => parsePlist(plist('<blob>1</blob>')), 'syntax');
throws('trailing', () => parsePlist('<true/><false/>'), 'syntax');
throws('dict value missing key', () => parsePlist(plist('<dict><string>x</string></dict>')), 'syntax');
throws('too deep', () => parsePlist(plist(`${'<array>'.repeat(80)}${'</array>'.repeat(80)}`)), 'deep');

// ---- a Sequel Ace favorites file ----
const fav = (fields) => `<dict>${Object.entries(fields).map(([k, v]) => `<key>${k}</key>${typeof v === 'number' ? `<integer>${v}</integer>` : typeof v === 'boolean' ? (v ? '<true/>' : '<false/>') : `<string>${v}</string>`}`).join('')}</dict>`;
const ACE = plist(`<dict>
  <key>Favorites Root</key>
  <dict>
    <key>Name</key><string>FAVORITES</string>
    <key>IsExpanded</key><true/>
    <key>Children</key>
    <array>
      ${fav({ id: 1001, name: 'prod-shop', host: 'db.example.net', port: '3306', user: 'shop_admin', database: 'shop', type: 0, colorIndex: 0, useSSL: 1,
    sslCACertFileLocation: '/Users/ana/certs/ca.pem', sslCACertFileLocationEnabled: 1, sslCertificateFileLocation: '/Users/ana/certs/client.pem', sslCertificateFileLocationEnabled: 0,
    sslKeyFileLocation: '/Users/ana/certs/client.key', sslKeyFileLocationEnabled: 1, password: 'hunter2', timeZone: 'UTC' })}
      <dict>
        <key>Name</key><string>Clients &amp; Co</string>
        <key>IsExpanded</key><false/>
        <key>Children</key>
        <array>
          ${fav({ id: '2002', name: 'local-dev', socket: '/tmp/mysql.sock', user: 'root', type: 1, colorIndex: 3, sshPassword: 'nope' })}
          <dict>
            <key>Name</key><string>Deep</string>
            <key>Children</key>
            <array>
              ${fav({ id: 3003, name: 'pi-lab', host: '192.168.1.40', port: 3307, user: 'pi', type: 2, sshHost: 'bastion.example.net', sshUser: 'deploy', sshPort: '2222',
    sshKeyLocation: '~/.ssh/id_ed25519', sshKeyLocationEnabled: 1, colorIndex: -1, sshPassphrase: 'x' })}
            </array>
          </dict>
        </array>
      </dict>
      ${fav({ id: 4004, name: 'staging-rds', host: 'shop-stg.cluster-c9xk2ab.us-east-1.rds.amazonaws.com', user: 'app_rw', type: 0, useAWSIAMAuth: true, awsProfile: 'staging', awsRegion: 'us-east-1', colorIndex: 1 })}
      ${fav({ id: 5005, name: '', host: '', user: 'root', type: 0, colorIndex: 12 })}
      <dict><key>Name</key><string>Empty group</string><key>Children</key><array/></dict>
    </array>
  </dict>
</dict>`);

const empty = { version: 1, groups: [], favorites: [] };
const imp = importSequel(ACE, { source: 'sequel-ace', existing: empty });
eq('found', [imp.found, imp.skipped, imp.favorites.length], [5, 0, 5]);
eq('groups', imp.groups.map((g) => [g.id, g.name, g.parent, g.expanded]), [['g1', 'Clients & Co', null, false], ['g2', 'Deep', 'g1', true]]);
eq('favorite groups', imp.favorites.map((f) => f.group), [null, 'g1', 'g2', null, null]);
eq('ids', imp.favorites.map((f) => f.id), ['prod-shop', 'local-dev', 'pi-lab', 'staging-rds', '127.0.0.1']);
const [p, l, s, c, e] = imp.favorites;
eq('tcp', [p.type, p.host, p.port, p.user, p.database, p.color, p.timeZone], ['tcp', 'db.example.net', 3306, 'shop_admin', 'shop', 0, 'UTC']);
eq('tls', p.tls, { mode: 'required', ca: '/Users/ana/certs/ca.pem', cert: '', key: '' });  // a key without its certificate is dropped
eq('imported', p.imported, { from: 'sequel-ace', id: '1001' });
eq('socket', [l.type, l.socket, l.host, l.color, l.tls.mode], ['socket', '/tmp/mysql.sock', '', 3, 'preferred']);
eq('ssh', [s.type, s.host, s.port, s.ssh, s.color], ['ssh', '192.168.1.40', 3307, { host: 'bastion.example.net', port: 2222, user: 'deploy', key: '~/.ssh/id_ed25519' }, null]);
eq('cloud iam', [c.type, c.cloud, c.color], ['cloud', { service: 'rds', region: 'us-east-1', iamProfile: 'staging' }, 1]);
eq('empty host and name', [e.host, e.name, e.color], ['127.0.0.1', '127.0.0.1', null]);
const json = JSON.stringify(imp);
eq('no password values', ['hunter2', 'nope', 'sshPassphrase', 'password'].some((x) => json.includes(x)), false);
eq('every imported favorite is valid', imp.favorites.map((f) => validateFavorite(cleanFavorite(f))), [[], [], [], [], []]);

// re-import: everything is skipped, nothing new
const merged = mergeImport(empty, imp);
eq('merged', [merged.version, merged.groups.length, merged.favorites.length], [1, 2, 5]);
const again = importSequel(ACE, { source: 'sequel-ace', existing: merged });
eq('reimport skipped', [again.found, again.skipped, again.favorites.length, again.groups.length], [5, 5, 0, 0]);
// the same file seen as Sequel Pro's is another source
const pro = importSequel(ACE, { source: 'sequel-pro', existing: merged });
eq('other source imports', [pro.favorites.length, pro.favorites[0].id, pro.favorites[0].imported.from], [5, 'prod-shop-2', 'sequel-pro']);
eq('other source reuses groups by name', pro.groups.length, 0);
eq('other source group ids', pro.favorites.map((f) => f.group), [null, 'g1', 'g2', null, null]);

// a new favorite added in Sequel Ace later: only that one comes in
const ACE2 = ACE.replace('<key>Children</key>\n    <array>', `<key>Children</key>\n    <array>${fav({ id: 6006, name: 'analytics', host: 'an.example.net', user: 'ro', type: 0 })}`);
const third = importSequel(ACE2, { source: 'sequel-ace', existing: merged });
eq('new one only', [third.found, third.skipped, third.favorites.map((f) => f.name)], [6, 5, ['analytics']]);
// duplicates inside one file are imported once
const dup = importSequel(plist(`<dict><key>Favorites Root</key><dict><key>Children</key><array>${fav({ id: 7, name: 'a', host: 'h' })}${fav({ id: 7, name: 'a', host: 'h' })}</array></dict></dict>`), { existing: empty });
eq('dup in file', [dup.found, dup.skipped, dup.favorites.length], [2, 1, 1]);

// a file that isn't favorites
throws('not favorites', () => importSequel(plist('<string>x</string>'), { existing: empty }), 'notfavorites');
throws('no children', () => importSequel(plist('<dict><key>Favorites Root</key><dict><key>Name</key><string>x</string></dict></dict>'), { existing: empty }), 'notfavorites');
// Sequel Pro's older layout: a bare array at the top
eq('bare array', importSequel(plist(`<array>${fav({ id: 1, name: 'x', host: 'h.example' })}</array>`), { existing: empty }).favorites.length, 1);

// field details
eq('port bounds', favoriteFrom({ host: 'h', port: 99999 }, 'sequel-ace').port, 65535);
eq('port string', favoriteFrom({ host: 'h', port: '' }, 'sequel-ace').port, 3306);
eq('newline stripped', favoriteFrom({ host: 'h', user: 'a\nb' }, 'sequel-ace').user, 'ab');
eq('cert pair kept', favoriteFrom({ host: 'h', sslCertificateFileLocation: '/c', sslCertificateFileLocationEnabled: 1, sslKeyFileLocation: '/k', sslKeyFileLocationEnabled: 1 }, 'x').tls, { mode: 'preferred', ca: '', cert: '/c', key: '/k' });
eq('ssh key disabled', favoriteFrom({ host: 'h', type: 2, sshHost: 'b', sshKeyLocation: '/k', sshKeyLocationEnabled: 0 }, 'x').ssh.key, '');

// where to look
const cands = candidatePaths('/home/ana', [{ path: '/media/ana/Macintosh HD', name: 'Macintosh HD' }], { '/media/ana/Macintosh HD': ['ana', 'Shared', '.localized', 'bob'] });
eq('candidates', cands.length, 2 + 4);
eq('candidate home', cands[0], { source: 'sequel-ace', path: `/home/ana/${SOURCES[0].rel}`, disk: '' });
eq('candidate disk', cands[2], { source: 'sequel-ace', path: `/media/ana/Macintosh HD/Users/ana/${SOURCES[0].rel}`, disk: 'Macintosh HD' });
eq('candidate pro', cands[5].path, `/media/ana/Macintosh HD/Users/bob/${SOURCES[1].rel}`);

console.log(`databases plist: ${ok} checks${process.exitCode ? ', FAILED' : ' passed'}`);
