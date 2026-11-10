// terminal_lsnames_test.mjs — the file name under the pointer in `ls` output (apps/terminal/lsnames.js).
import { nameAt, promptDir, titleDir, expandHome } from '../../apps/terminal/lsnames.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};
// the pointer on the n-th character of `needle` in `line`
const at = (line, needle, n = 0) => { const i = line.indexOf(needle) + n; return nameAt(line.slice(0, i), line.slice(i)); };

const L1 = '-rw-r--r--  1 glk1 glk1 3000000 Nov  9 22:07 big.bin';
eq('long: a file', at(L1, 'big.bin', 2), { name: 'big.bin', dir: false });
eq('long: on the size', at(L1, '3000000'), null);
eq('long: on the date', at(L1, 'Nov'), null);
const L2 = 'drwxr-xr-x  3 glk1 glk1    4096 Nov  9 22:07 folder';
eq('long: a folder', at(L2, 'folder'), { name: 'folder', dir: true });
const L3 = "-rw-r--r--  1 glk1 glk1       5 Nov  9 22:07 'notes (2).txt'";
eq('long: a quoted name with spaces', at(L3, 'notes', 3), { name: 'notes (2).txt', dir: false });
const L4 = '-rw-r--r--  1 glk1 glk1       5 Nov  9 22:07 notes (2).txt';
eq('long: an unquoted name with spaces (ls -N)', at(L4, '(2)'), { name: 'notes (2).txt', dir: false });
const L5 = 'lrwxrwxrwx  1 glk1 glk1      15 Nov  9 22:11 secrets -> /home/glk1/.ssh';
eq('long: a link is its own name', at(L5, 'secrets'), { name: 'secrets', dir: false });
const L6 = '-rw-r--r-- 1 root root 1234 2026-11-09 22:07 iso.log';
eq('long: --time-style=long-iso', at(L6, 'iso.log'), { name: 'iso.log', dir: false });
const L7 = '-rw-r--r-- 1 root root 1.2K Mar  3  2025 old.txt';
eq('long: -h size and a year', at(L7, 'old.txt'), { name: 'old.txt', dir: false });
const L8 = 'crw-rw-rw- 1 root root 1, 3 Nov  9 10:00 null';
eq('long: a device', at(L8, 'null'), { name: 'null', dir: false });
const L9 = '-rw-r--r--. 1 a b 10 Nov  9 22:07 selinux.txt';
eq('long: an SELinux dot', at(L9, 'selinux'), { name: 'selinux.txt', dir: false });

const C1 = "big.bin  folder  'notes (2).txt'  notes.txt";
eq('columns: first', at(C1, 'big'), { name: 'big.bin', dir: false });
eq('columns: middle', at(C1, 'folder', 3), { name: 'folder', dir: false });
eq('columns: quoted', at(C1, '(2)'), { name: 'notes (2).txt', dir: false });
eq('columns: last', at(C1, 'notes.txt', 8), { name: 'notes.txt', dir: false });
eq('columns: between names', nameAt('big.bin ', ' folder'), null);
eq('columns: total line', at('total 2952', '2952'), null);
eq('empty line', nameAt('', ''), null);
eq('past the end', nameAt('abc', ''), null);
eq('dot entries are not names', at('drwxr-xr-x  3 a b 4096 Nov  9 22:07 ..', '..'), null);
eq('a quote escape', at("x  'it'\\''s'", 'it'), { name: "it's", dir: false });

// where that ls ran, on a server without the LordOS integration (the Pi's case: ~/Downloads, the file not in ~)
const PI = [
  'lordbasex@lordos-pi:~$ cd Downloads',
  'lordbasex@lordos-pi:~/Downloads$ ls -la',
  'total 720184',
  'drwxr-xr-x  2 lordbasex lordbasex      4096 nov 10 07:18 .',
  '-rw-rw-r--  1 lordbasex lordbasex 737456128 ago  9  2025 debian-13.0.0-arm64-netinst.iso',
  'lordbasex@lordos-pi:~/Downloads$ cd /etc',
  'lordbasex@lordos-pi:/etc$ ',
];
eq('the prompt above the clicked line', promptDir(PI, PI[4]), '~/Downloads');
eq('not the current prompt below it', promptDir(PI, PI[4]) !== '/etc', true);
eq('a line not on the screen: the last prompt', promptDir(PI, 'gone'), '/etc');
eq('root and an absolute path', promptDir(['root@srv:/var/log# ls', 'syslog'], 'syslog'), '/var/log');
eq('a virtualenv before the prompt', promptDir(['(venv) ana@box:~/src$ ls', 'a.py'], 'a.py'), '~/src');
eq('a directory with spaces', promptDir(['ana@box:~/My Files$ ls', 'a.txt'], 'a.txt'), '~/My Files');
eq('no prompt on the screen', promptDir(['a.txt', 'b.txt'], 'a.txt'), '');
eq('trailing spaces on the screen lines', promptDir(['ana@box:~/x$ ls   ', 'a.txt   '], 'a.txt'), '~/x');
eq('Debian\'s title', titleDir('lordbasex@lordos-pi: ~/Downloads'), '~/Downloads');
eq('a title with an absolute path', titleDir('root@srv: /etc'), '/etc');
eq('a title that is not a directory', titleDir('vim notes.txt'), '');
eq('a title with a control character', titleDir('a@b: /x\x07y'), '');
eq('~ is the home', expandHome('~', '/home/ana'), '/home/ana');
eq('~/ under the home', expandHome('~/Downloads', '/home/ana/'), '/home/ana/Downloads');
eq('no home known', expandHome('~/x', ''), '');
eq('an absolute path stays', expandHome('/etc', '/home/ana'), '/etc');

console.log(`terminal_lsnames_test: ${ok}/${ok + bad} checks`);
if (bad) process.exit(1);
