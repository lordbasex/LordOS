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
eq('host after user info', urlHost('https://user@bank.example/x'), 'bank.example');
eq('IPv6 host', urlHost('http://[::1]:8080/'), '[::1]');
eq('not a web address', urlHost('ftp://example.org'), '');

eq('a file and its folder', splitPath('/home/ana/a.txt'), { name: 'a.txt', dir: '/home/ana' });
eq('at the root', splitPath('/a.txt'), { name: 'a.txt', dir: '/' });
eq('a folder with a slash', splitPath('/home/ana/Music/'), { name: 'Music', dir: '/home/ana' });

console.log(`portal_open_model_test: ${ok}/${ok + bad} checks`);
if (bad) process.exit(1);
