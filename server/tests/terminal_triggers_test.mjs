// terminal_triggers_test.mjs — a trigger tried on a sample line (apps/terminal/triggers.js).
import { triggerPreview, previewLine, TRIGGER_ACTIONS } from '../../apps/terminal/triggers.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};

eq('an invalid expression', triggerPreview('(', '', 'x'), { error: true });
eq('an empty expression', triggerPreview('', '', 'x'), { error: true });
eq('no match', triggerPreview('error', '', 'all good'), { match: null });
eq('a match with groups', triggerPreview('build (\\w+) in (\\d+)s', 'Build \\1 took \\2 s', 'build failed in 42s'),
  { match: 'build failed in 42s', groups: ['failed', '42'], result: 'Build failed took 42 s' });
eq('\\0 is the whole match', triggerPreview('ERR\\d+', 'got \\0', 'x ERR42 y').result, 'got ERR42');
eq('a missing group is empty', triggerPreview('(a)|(b)', '[\\2]', 'a').result, '[]');
eq('a long sample is cut', triggerPreview('end$', '', `${'x'.repeat(5000)}end`), { match: null });
eq('notify shows the result', previewLine('notify', triggerPreview('done', 'Finished', 'all done'), 'all done'), { kind: 'notice', text: 'Finished' });
eq('notify without a parameter shows the line', previewLine('notify', triggerPreview('done', '', '  all done '), '  all done '), { kind: 'notice', text: 'all done' });
eq('title without a parameter is the match', previewLine('title', triggerPreview('v\\d+', '', 'release v12'), 'release v12'), { kind: 'title', text: 'v12' });
eq('send', previewLine('send', triggerPreview('Password:', 'secret\\n', 'Password:'), 'Password:'), { kind: 'send', text: 'secret\\n' });
eq('no line without a match', previewLine('notify', triggerPreview('x', '', 'y'), 'y'), null);
eq('every action has an id', TRIGGER_ACTIONS.every((a) => typeof a.id === 'string'), true);

console.log(`terminal_triggers_test: ${ok}/${ok + bad} checks`);
if (bad) process.exit(1);
