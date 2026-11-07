// terminal_paths_test.mjs — (0.8.8.6) files into the Terminal as text (apps/terminal/paths.js): dropped files
// and the clipboard's image go in as quoted paths. Run by tests/run_suites.sh.
import { shellQuote, pathsText, clipboardImage, programLabel, isClaude } from '../../apps/terminal/paths.js';

let n = 0, bad = 0;
const eq = (what, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${what}\n    got  ${g}\n    want ${w}`); }
};

eq('a plain path stays as is', shellQuote('/home/ana/Pictures/shot.png'), '/home/ana/Pictures/shot.png');
eq('spaces are quoted', shellQuote('/home/ana/Mis fotos/a b.png'), "'/home/ana/Mis fotos/a b.png'");
eq("a quote inside", shellQuote("/tmp/it's.png"), "'/tmp/it'\\''s.png'");
eq('shell characters are quoted', shellQuote('/tmp/$(rm -rf ~).png'), "'/tmp/$(rm -rf ~).png'");
eq('several files, then a space', pathsText(['/a.png', '/b c.png']), "/a.png '/b c.png' ");
eq('the clipboard image', clipboardImage({ kind: 'image', image: '/run/user/1000/lordos/clip-3.png' }), '/run/user/1000/lordos/clip-3.png');
eq('text on the clipboard', clipboardImage({ kind: 'text', image: '' }), null);
eq('Claude Code by name', programLabel('claude'), 'Claude Code');
eq('another program keeps its name', programLabel('vim'), 'vim');
eq('claude is recognized', isClaude('claude') && !isClaude('bash'), true);

console.log(`terminal_paths_test: ${n - bad}/${n} checks`);
process.exit(bad ? 1 : 0);
