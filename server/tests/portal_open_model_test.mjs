// portal_open_model_test.mjs — what the shell does with a Linux app's OpenURI request (sdk/portalopenmodel.js).
import { openPlan, urlHost, splitPath } from '../../sdk/portalopenmodel.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};
const refuse = { action: 'refuse', ask: false, target: '' };

eq('a web address from the app in front', openPlan({ kind: 'uri', target: 'https://example.org/a?b=1', front: true }),
  { action: 'url', ask: false, target: 'https://example.org/a?b=1' });
eq('a web address from an app in the background asks', openPlan({ kind: 'uri', target: 'http://example.org', front: false }),
  { action: 'url', ask: true, target: 'http://example.org' });
eq('front unknown asks', openPlan({ kind: 'uri', target: 'https://example.org' }).ask, true);
eq('front must be exactly true', openPlan({ kind: 'uri', target: 'https://example.org', front: 1 }).ask, true);
eq('upper-case scheme', openPlan({ kind: 'uri', target: 'HTTPS://Example.org', front: true }).action, 'url');
eq('mailto is not opened', openPlan({ kind: 'uri', target: 'mailto:a@example.org', front: true }), refuse);
eq('javascript is not opened', openPlan({ kind: 'uri', target: 'javascript:alert(1)', front: true }), refuse);
eq('another app scheme is not opened', openPlan({ kind: 'uri', target: 'steam://run/1', front: true }), refuse);
eq('no host', openPlan({ kind: 'uri', target: 'https://', front: true }), refuse);
eq('spaces', openPlan({ kind: 'uri', target: 'https://example.org/a b', front: true }), refuse);
eq('too long', openPlan({ kind: 'uri', target: `https://example.org/${'a'.repeat(5000)}`, front: true }), refuse);
eq('a file', openPlan({ kind: 'file', target: '/home/ana/notes.txt', front: true }),
  { action: 'file', ask: false, target: '/home/ana/notes.txt' });
eq('a folder from the background', openPlan({ kind: 'folder', target: '/home/ana/Music', front: false }),
  { action: 'folder', ask: true, target: '/home/ana/Music' });
eq('reveal', openPlan({ kind: 'reveal', target: '/home/ana/a.pdf', front: true }).action, 'reveal');
eq('a relative path', openPlan({ kind: 'file', target: 'notes.txt', front: true }), refuse);
eq('an unknown kind', openPlan({ kind: 'exec', target: '/bin/sh', front: true }), refuse);
eq('nothing', openPlan(null), refuse);
eq('no target', openPlan({ kind: 'uri', front: true }), refuse);

eq('host', urlHost('https://Example.org:8443/x'), 'example.org');
eq('user info is refused', urlHost('https://user@bank.example/x'), '');
eq('IPv6 host', urlHost('http://[::1]:8080/'), '[::1]');
eq('not a web address', urlHost('ftp://example.org'), '');

// Addresses a browser could read differently from the question (parser differential): all refused
const tricky = [
  ['user info', 'https://bank.example@evil.example/'],
  ['user and password', 'https://user:pw@example.org/'],
  ['an @ in the port place', 'https://bank.example:443@evil.example'],
  ['backslash as a slash', 'https://evil.example\\@bank.example/'],
  ['backslash after the host', 'https://bank.example\\.evil.example/'],
  ['backslash in the path', 'https://example.org/a\\b'],
  ['a tab inside the host', 'https://ban\tk.example/'],
  ['a newline', 'https://example.org/\nx'],
  ['a NUL', 'https://example.org/\u0000'],
  ['DEL', 'https://example.org/\u007f'],
  ['a C1 control', 'https://example.org/\u0085'],
  ['a leading space', ' https://example.org/'],
  ['a no-break space', 'https://example.org/ '],
  ['raw international host', 'https://bäcker.example/'],
  ['raw international path', 'https://example.org/café'],
  ['a percent in the host', 'https://evil%2Eexample/'],
  ['an encoded @ in the host', 'https://bank.example%40evil.example/'],
  ['a decimal IPv4', 'http://2130706433/'],
  ['a hexadecimal IPv4', 'http://0x7f.1/'],
  ['an octal IPv4', 'http://0177.0.0.1/'],
  ['a short IPv4', 'http://127.1/'],
  ['an IPv4 with a trailing dot', 'http://1.2.3.4./'],
  ['an IPv4 out of range', 'http://1.2.3.256/'],
  ['a name ending in a number', 'http://example.123/'],
  ['an empty port', 'https://example.org:/'],
  ['a port that is not a number', 'https://example.org:443x/'],
  ['a port too big', 'https://example.org:65536/'],
  ['two colons', 'https://example.org:1:2/'],
  ['an empty label', 'https://example..org/'],
  ['a broken IPv6', 'http://[1:2:3]/'],
  ['two :: in an IPv6', 'http://[1::2::3]/'],
  ['an IPv6 without its bracket', 'http://[::1/'],
  ['an IPv6 followed by a name', 'http://[::1]evil.example/'],
  ['a single slash', 'https:/example.org/'],
  ['three slashes', 'https:///example.org/'],
  ['a blob', 'blob:https://example.org/x'],
  ['data', 'data:text/html,<b>x</b>'],
  ['file is not a web address', 'file:///etc/passwd'],
];
for (const [label, url] of tricky) {
  eq(`refused: ${label}`, openPlan({ kind: 'uri', target: url, front: true }), refuse);
  eq(`no host: ${label}`, urlHost(url), '');
}

// Addresses that pass show the host the WHATWG parser (node's URL) finds
const fine = [
  ['https://example.org/a?b=1#c', 'example.org'],
  ['HTTPS://WWW.Example.ORG:8443/x', 'www.example.org'],
  ['http://example.org', 'example.org'],
  ['http://example.org.', 'example.org.'],
  ['http://example.org?q=@x', 'example.org'],
  ['http://example.org#@evil.example', 'example.org'],
  ['https://example.org/path@evil.example', 'example.org'],
  ['https://xn--bcker-gra.example/', 'xn--bcker-gra.example'],
  ['https://my_host.example/', 'my_host.example'],
  ['http://192.168.1.10:8080/', '192.168.1.10'],
  ['http://[::1]:8080/', '[::1]'],
  ['http://[2001:db8::1]/', '[2001:db8::1]'],
  ['http://[::ffff:192.168.0.1]/', '[::ffff:c0a8:1]'],
  ['https://example.org/a%20b', 'example.org'],
];
for (const [url, host] of fine) {
  const mine = urlHost(url);
  eq(`accepted: ${url}`, openPlan({ kind: 'uri', target: url, front: true }).action, 'url');
  eq(`same host as URL: ${url}`, new URL(url).hostname, host);
  if (mine[0] !== '[') eq(`shown host: ${url}`, mine, host);
}

// Without URL (lord-server's QuickJS) the answers are the same
const savedURL = globalThis.URL;
const all = [...tricky.map((x) => x[1]), ...fine.map((x) => x[0])];
const withURL = all.map((u) => [urlHost(u), openPlan({ kind: 'uri', target: u, front: true }).action]);
globalThis.URL = undefined;
const withoutURL = all.map((u) => [urlHost(u), openPlan({ kind: 'uri', target: u, front: true }).action]);
globalThis.URL = savedURL;
eq('the same answers without URL', withoutURL, withURL);

eq('a file and its folder', splitPath('/home/ana/a.txt'), { name: 'a.txt', dir: '/home/ana' });
eq('at the root', splitPath('/a.txt'), { name: 'a.txt', dir: '/' });
eq('a folder with a slash', splitPath('/home/ana/Music/'), { name: 'Music', dir: '/home/ana' });

console.log(`portal_open_model_test: ${ok}/${ok + bad} checks`);
if (bad) process.exit(1);
