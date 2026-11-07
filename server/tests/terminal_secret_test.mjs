// terminal_secret_test.mjs — (0.8.8.6) Passwords in the Terminal, the pure part (apps/terminal/secret.js): the
// host a password prompt is for, and the Passwords entries offered, that host's first. Run by run_suites.sh.
import { secretHost, rankEntries } from '../../apps/terminal/secret.js';

let n = 0, bad = 0;
const eq = (what, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${what}\n    got  ${g}\n    want ${w}`); }
};

eq('ssh', secretHost("ana@Build.Example.org's password:"), 'build.example.org');
eq('git over https', secretHost("Password for 'https://ana@github.com':"), 'github.com');
eq('sudo names no host: the session host', secretHost('[sudo] password for ana:', 'ana@srv1.lan'), 'srv1.lan');
eq('a local session has no host', secretHost('[sudo] password for ana:', 'ana@localhost'), '');
eq('nothing at all', secretHost('Enter passphrase for key:', ''), '');

const entries = [
  { id: 'a', cat: 'internet', title: 'GitHub', sub: 'ana', url: 'https://github.com/login' },
  { id: 'b', cat: 'internet', title: 'Build server', sub: 'ana', url: 'ssh://build.example.org' },
  { id: 'c', cat: 'internet', title: 'Bank', url: 'https://bank.example' },
  { id: 'd', cat: 'card', title: 'Visa' },
  { id: 'e', cat: 'internet', title: 'Old build', url: 'build.example.org', trash: 1700000000 },
];
eq('the host first, then by title; no cards, no trash', rankEntries(entries, 'build.example.org').map((e) => [e.id, e.match]),
  [['b', true], ['c', false], ['a', false]]);
eq('a subdomain of the saved address matches', rankEntries(entries, 'api.github.com')[0].id, 'a');
eq('no host: all logins by title', rankEntries(entries, '').map((e) => e.id), ['c', 'b', 'a']);
eq('only titles and user names leave the list', Object.keys(rankEntries(entries, '')[0]), ['id', 'title', 'sub', 'match']);

console.log(`terminal_secret_test: ${n - bad}/${n} checks`);
process.exit(bad ? 1 : 0);
