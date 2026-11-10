// terminal_profilefields_test.mjs — "Send text at start" and the tags field (apps/terminal/profilefields.js).
import { initialKeys, parseTags, tagsText } from '../../apps/terminal/profilefields.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};

eq('nothing to send', initialKeys(''), '');
eq('only spaces and breaks', initialKeys(' \n\n '), '');
eq('a command runs with its Enter', initialKeys('ssh pi'), 'ssh pi\r');
eq('one Enter, not two, after a line break at the end', initialKeys('ssh pi\n'), 'ssh pi\r');
eq('several breaks at the end are one Enter', initialKeys('ssh pi\n\n\n'), 'ssh pi\r');
eq('two lines are two commands', initialKeys('cd /srv\nls -l'), 'cd /srv\rls -l\r');
eq('a "\\n" saved before 0.8.8.8 is a break', initialKeys('ssh pi\\n'), 'ssh pi\r');
eq('a "\\n" in the middle too', initialKeys('cd /srv\\nls'), 'cd /srv\rls\r');
eq('CR LF from a paste', initialKeys('a\r\nb\r\n'), 'a\rb\r');
eq('an imported iTerm2 command and text', initialKeys('ssh -o StrictHostKeyChecking=no root@10.0.0.1\nsudo -i'), 'ssh -o StrictHostKeyChecking=no root@10.0.0.1\rsudo -i\r');
eq('leading spaces kept (HISTCONTROL=ignorespace)', initialKeys('  secret-cmd'), '  secret-cmd\r');

eq('one tag', parseTags('ATAM - VIVELIBRE'), ['ATAM - VIVELIBRE']);
eq('two tags, spaces trimmed', parseTags(' Customers ,Production '), ['Customers', 'Production']);
eq('empty pieces dropped', parseTags('a,, ,b,'), ['a', 'b']);
eq('no tags', parseTags(''), []);
eq('the same tag twice, ignoring case, once', parseTags('Prod, prod, PROD'), ['Prod']);
eq('control characters dropped', parseTags('a\tb,\x07c'), ['ab', 'c']);
eq('a long tag cut', parseTags('x'.repeat(300))[0].length, 100);
eq('the field shows the list', tagsText(['ATAM - VIVELIBRE', 'Pi']), 'ATAM - VIVELIBRE, Pi');
eq('the field skips what is not a tag', tagsText(['a', '', 3, ' ']), 'a');
eq('no list', tagsText(undefined), '');
eq('round trip', parseTags(tagsText(['a', 'b c'])), ['a', 'b c']);

console.log(`terminal_profilefields_test: ${ok}/${ok + bad} checks`);
if (bad) process.exit(1);
