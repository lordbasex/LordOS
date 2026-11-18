// claude_widget_model_test.mjs — the Claude Code widget's plain logic (sdk/claudewidgetmodel.js).
import { configDirs, accountOf, leftText, alertOf, summarize } from '../../sdk/claudewidgetmodel.js';

let ok = 0, bad = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) === JSON.stringify(b)) ok++; else { bad++; console.error(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); } };
eq('config folders: the default first, other accounts after', configDirs(['.cache', '.claude-cuenta2', '.claude', '.claude.json', '.claude-x/../y', '.config']), ['.claude', '.claude-cuenta2']);
eq('account: name, e-mail, plan only', accountOf(JSON.stringify({ oauthAccount: { displayName: 'Fede', emailAddress: 'a@b.c', planDisplayName: 'Ultra', accountUuid: 'x' }, other: 1 })), { name: 'Fede', email: 'a@b.c', plan: 'Ultra' });
eq('account: the name from the e-mail', accountOf(JSON.stringify({ oauthAccount: { emailAddress: 'work@x.org' } })), { name: 'work', email: 'work@x.org', plan: '' });
eq('account: none', [accountOf('{}'), accountOf('nope'), accountOf(JSON.stringify({ oauthAccount: {} }))], [null, null, null]);
const t = (k, v) => `${k.split('.').pop()}:${JSON.stringify(v)}`;
eq('time left', [leftText(2 * 86400000 + 4 * 3600000, t), leftText(3 * 3600000 + 20 * 60000, t), leftText(12 * 60000, t), leftText(0, t)],
  ['d:{"d":2,"h":4}', 'h:{"h":3,"m":20}', 'm:{"m":12}', '']);
eq('alerts', [alertOf({ ctx: 50 }), alertOf({ ctx: 72 }), alertOf({ week: { pct: 90 } }), alertOf({ five: { pct: 86 } }), alertOf(null)], ['', 'warn', 'high', 'high', '']);
const r = summarize([
  { id: 'a', dir: '.claude', state: 'waiting', ctx: 30, week: { pct: 38 }, at: 2 },
  { id: 'b', dir: '.claude', state: 'working', ctx: 80, week: { pct: 39 }, five: { pct: 12 }, at: 5 },
  { id: 'c', dir: '.claude-cuenta2', state: '', ctx: 10, week: { pct: 5 }, at: 1 },
  { id: 'bad id!', dir: '.claude' },
], { '.claude': { name: 'Fede' } });
eq('the working session first', r.sessions.map((s) => s.id), ['b', 'a', 'c']);
eq('per account: the newest limits and how many sessions', r.accounts.map((a) => [a.dir, a.sessions, a.week?.pct, a.five?.pct, a.account?.name || null]),
  [['.claude', 2, 39, 12, 'Fede'], ['.claude-cuenta2', 1, 5, null, null]]);
console.log(bad ? `claude-widget-model-test: ${bad} FAILED` : `claude-widget-model-test: ${ok} ok`);
if (bad) process.exit(1);
