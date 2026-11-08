// terminal_claude_test.mjs — (0.8.8.6) Claude Code in the Terminal's footer, the pure part
// (apps/terminal/claude.js): the session file, the status line JSON, what fits, and connecting the status line
// without changing what it shows. Run by run_suites.sh.
import { execFileSync } from 'node:child_process';
import {
  BRIDGE, parseSession, parseStatus, level, bar, tokens, fitSegments, wrapCommand, unwrapCommand, isConnected,
  connectSettings, disconnectSettings, parseSettings,
} from '../../apps/terminal/claude.js';

let n = 0, bad = 0;
const eq = (what, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${what}\n    got  ${g}\n    want ${w}`); }
};

// ---- the session file ----
const sess = (o) => JSON.stringify({ pid: 100448, sessionId: 'e9be29c2-84cd-4d07-b0e2-348c94977132', cwd: '/x', status: 'busy', ...o });
eq('working', parseSession(sess(), 100448), { id: 'e9be29c2-84cd-4d07-b0e2-348c94977132', state: 'working' });
eq('waiting', parseSession(sess({ status: 'idle' }), 100448)?.state, 'waiting');
eq('an unknown status shows no state', parseSession(sess({ status: 'other' }), 100448)?.state, '');
eq('another process\'s file (a reused pid)', parseSession(sess(), 4242), null);
eq('a session id that could leave the folder', parseSession(sess({ sessionId: '../../x' }), 100448), null);
eq('not JSON', parseSession('{', 100448), null);

// ---- the status line JSON ----
const now = 1794100000000;
const st = (o) => JSON.stringify({
  session_id: 'S1', model: { display_name: 'Opus 5.5' },
  context_window: { used_percentage: 23.6, total_input_tokens: 236989, context_window_size: 1000000 },
  rate_limits: { five_hour: { used_percentage: 37, resets_at: now / 1000 + 3600 }, seven_day: { used_percentage: 61.2, resets_at: now / 1000 + 86400 } },
  cost: { total_cost_usd: 12.5 }, ...o,
});
eq('all of it', parseStatus(st(), 'S1', now), {
  model: 'Opus 5.5', ctx: 24, ctxTokens: 236989, ctxSize: 1000000,
  five: { pct: 37, resetsAt: now + 3600000 }, week: { pct: 61, resetsAt: now + 86400000 }, cost: 12.5,
});
eq('another session\'s snapshot', parseStatus(st(), 'S2', now), null);
eq('a window that already reset shows as unknown', parseStatus(st({ rate_limits: { five_hour: { used_percentage: 90, resets_at: now / 1000 - 1 } } }), 'S1', now).five, { pct: null, resetsAt: null });
eq('a subscriber without the 5-hour window keeps its slot', parseStatus(st({ rate_limits: { seven_day: { used_percentage: 5, resets_at: now / 1000 + 86400 } } }), 'S1', now).five, { pct: null, resetsAt: null });
eq('no limits (an API key account)', parseStatus(st({ rate_limits: undefined }), 'S1', now).week, null);
eq('no context before the first answer', parseStatus(st({ context_window: { used_percentage: null } }), 'S1', now).ctx, null);
eq('percentages stay within 0..100', parseStatus(st({ context_window: { used_percentage: 140 } }), 'S1', now).ctx, 100);
eq('a long model name is cut', parseStatus(st({ model: { display_name: 'x'.repeat(80) } }), 'S1', now).model.length, 32);

// ---- how it's shown ----
eq('levels', [level(10), level(50), level(79), level(80), level(null)], ['ok', 'warn', 'warn', 'high', 'none']);
eq('bar', [bar(0), bar(1), bar(24), bar(50), bar(100)], ['▱▱▱▱▱', '▰▱▱▱▱', '▰▱▱▱▱', '▰▰▰▱▱', '▰▰▰▰▰']);
eq('tokens', [tokens(950), tokens(236989), tokens(1000000), tokens(1500000)], ['950', '237k', '1M', '1.5M']);
const all = { state: 1, ctx: 1, five: 1, week: 1, model: 1 };
eq('a wide window: everything', fitSegments(227, all), ['state', 'ctx', 'five', 'week', 'model']);
eq('a narrow one: state and context first', fitSegments(60, all), ['state', 'ctx']);
eq('only what there is', fitSegments(227, { state: 1, ctx: 0, five: 1 }), ['state', 'five']);

// ---- connecting the status line ----
const mine = 'bash ~/.claude/statusline-command.sh';
eq('a simple command: the bridge in front', wrapCommand(mine), `${BRIDGE} ${mine}`);
eq('no status line: the bridge alone', wrapCommand(''), BRIDGE);
const piped = `jq -r '.model.display_name' | tr a-z A-Z`;
eq('shell syntax: whole, through sh -c', wrapCommand(piped), `${BRIDGE} sh -c 'jq -r '\\''.model.display_name'\\'' | tr a-z A-Z'`);
for (const c of [mine, piped, '', 'echo "$HOME" && date', "printf '%s' it's"]) eq(`round trip: ${c}`, unwrapCommand(wrapCommand(c)), c);
// the wrapped command runs as the original did (env stands in for the bridge)
const run = (cmd) => execFileSync('sh', ['-c', cmd], { input: '{"x":1}', encoding: 'utf8' });
for (const c of [`printf 'a b' && cat`, `cat | tr x y`, `echo "it's"; echo $((1+2))`])
  eq(`same output: ${c}`, run(wrapCommand(c).replace(BRIDGE, 'env')), run(c));
const settings = { model: 'opus', statusLine: { type: 'command', command: mine, padding: 2 }, permissions: { allow: ['Bash(ls)'] } };
const on = connectSettings(settings);
eq('connected', isConnected(on), true);
eq('the rest of the settings untouched', { ...on, statusLine: undefined }, { ...settings, statusLine: undefined });
eq('the status line keeps its options', on.statusLine.padding, 2);
eq('connecting twice changes nothing', connectSettings(on), on);
eq('disconnecting gives the original back', disconnectSettings(on), settings);
eq('no status line before: none after', disconnectSettings(connectSettings({ model: 'x' })), { model: 'x' });
eq('the original settings object is not changed', settings.statusLine.command, mine);
eq('empty settings file', parseSettings(''), {});
eq('unreadable settings are not overwritten', parseSettings('{ broken'), null);

console.log(`${n - bad}/${n} terminal claude checks passed`);
if (bad) process.exit(1);
