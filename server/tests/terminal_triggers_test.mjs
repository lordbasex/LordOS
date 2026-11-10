// terminal_triggers_test.mjs — a trigger tried on a sample line (apps/terminal/triggers.js).
import { triggerPreview, previewLine, TRIGGER_ACTIONS, screenSnapshot, freshLines } from '../../apps/terminal/triggers.js';

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

// which screen lines are new (freshLines): a screen of `rows` rows whose first row is `top`, one text per row
const scr = (top, texts, cols = 80) => screenSnapshot(texts.map((x, i) => `${top + i}\t${x}`).join('\n'), { cols, rows: texts.length });
eq('a snapshot from the engine', screenSnapshot('7\tabc\n9\tx\ty\n\nbad', { cols: 5, rows: 3 }),
  { cols: 5, rows: 3, lines: [{ row: 7, text: 'abc' }, { row: 9, text: 'x\ty' }] });
eq('the first look tries every line', freshLines(null, scr(0, ['$ make', 'error 1', '', ''])), ['$ make', 'error 1']);
eq('nothing changed', freshLines(scr(0, ['$ make', 'error 1', '$ ']), scr(0, ['$ make', 'error 1', '$ '])), []);
eq('the same line printed again fires again',
  freshLines(scr(0, ['$ echo error', 'error', '$ echo error', '']), scr(0, ['$ echo error', 'error', '$ echo error', 'error'])), ['error']);
eq('printed twice at once fires twice',
  freshLines(scr(0, ['$ x', '', '', '']), scr(0, ['$ x', 'error', 'error', '$ '])), ['error', 'error', '$ ']);
eq('a scroll alone fires nothing', freshLines(scr(0, ['a', 'error', 'b', 'c']), scr(2, ['b', 'c', '', ''])), []);
eq('a scroll that brings the same text again fires it',
  freshLines(scr(0, ['error', '$ echo error', 'error', '$ ']), scr(2, ['error', '$ ', 'error', '$ '])), ['error', '$ ']);
eq('the old line scrolled away, the new one fires', freshLines(scr(0, ['error', 'b', 'c']), scr(3, ['d', 'e', 'error'])), ['d', 'e', 'error']);
eq('clear, then the line again', freshLines(scr(0, ['$ echo error', 'error', '$ clear']), scr(3, ['$ echo error', 'error', ''])),
  ['$ echo error', 'error']);
eq('a partial redraw fires only the changed line',
  freshLines(scr(0, ['done 1', 'progress 40%', '']), scr(0, ['done 1', 'progress 41%', ''])), ['progress 41%']);
eq('a line typed over itself fires once it changes', freshLines(scr(0, ['$ ech']), scr(0, ['$ echo error'])), ['$ echo error']);
eq('a full-screen program moving lines up fires only the new one',
  freshLines(scr(0, ['line 1', 'error 2', 'line 3']), scr(0, ['error 2', 'line 3', 'error 4'])), ['error 4']);
eq('a full-screen program moving the same text keeps it once',
  freshLines(scr(0, ['error', 'x', 'y']), scr(0, ['x', 'error', 'y'])), []);
eq('a resize goes by how many times each text was there',
  freshLines(scr(0, ['error', 'error', 'ok']), scr(5, ['error', 'ok', 'error', 'error'], 40)), ['error']);

console.log(`terminal_triggers_test: ${ok}/${ok + bad} checks`);
if (bad) process.exit(1);
