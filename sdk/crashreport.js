// crashreport.js — the text "Copy details" puts on the clipboard when an app stops at its own window (AppBoundary in
// components.jsx): enough for a GitHub issue (app, LordOS version, the error and where it happened), nothing personal.
// Paths under a home folder are shortened to ~ so a report doesn't carry the user's name.

const MAX_LINES = 12;

const tidy = (s) => String(s || '').replace(/\/home\/[^/\s:)]+/g, '~').replace(/\/root(?=\/)/g, '~');
const firstLines = (s, n) => tidy(s).split('\n').map((l) => l.trimEnd()).filter((l) => l.trim()).slice(0, n);

export function crashReport({ app = '', name = '', version = '', theme = '', error = null, componentStack = '' } = {}) {
  const msg = tidy(error?.message || (error == null ? '' : String(error))) || 'unknown error';
  const out = [
    `App: ${name ? `${name} (${app})` : app}`,
    `LordOS: ${version || 'unknown'}`,
  ];
  if (theme) out.push(`Theme: ${theme}`);
  out.push(`Error: ${msg}`);
  const stack = firstLines(error?.stack, MAX_LINES + 1);
  if (stack.length && (stack[0].trim() === msg || stack[0].trim().endsWith(`: ${msg}`))) stack.shift();   // V8's "Error: msg" line
  stack.length = Math.min(stack.length, MAX_LINES);
  if (stack.length) out.push('', 'Stack:', ...stack.map((l) => `  ${l.trim()}`));
  const comp = firstLines(componentStack, MAX_LINES);
  if (comp.length) out.push('', 'Components:', ...comp.map((l) => `  ${l.trim()}`));
  return out.join('\n');
}
