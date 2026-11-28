// writingmodel.js — "✨ AI" on selected text, the pure part (docs/AI.md "Writing"; tested by
// server/tests/writing_model_test.mjs): when the menu offers it, its rows, the panel's states, and the model's answer
// made plain text.
//
// The shell runs it, never the app: a text field hands over its selection and gets the final text back only when the
// person clicks Replace. The prompts are fixed in lord-server (server/src/llm/writing.c); nothing here builds one.

export const WRITE_ACTIONS = ['fix', 'improve', 'shorter'];
export const WRITE_MAX_CHARS = 4000;          // the selection; the engine's context is 4,096 tokens (docs/SENTINELA.md)
// the translation targets: LordOS's interface languages (writing.c's closed list)
export const TRANSLATE_LANGS = ['ar', 'az', 'bg', 'bn', 'ca', 'de', 'el', 'en', 'es', 'fa', 'fi', 'fil', 'fr', 'hi', 'hu', 'id', 'it', 'ja',
  'ko', 'lt', 'ms', 'nl', 'pl', 'pt', 'ro', 'ru', 'sv', 'sw', 'th', 'tr', 'uk', 'ur', 'vi', 'zh-Hans', 'zh-Hant'];
const COMMON = ['en', 'es', 'pt', 'fr', 'de', 'it', 'zh-Hans', 'ja'];
export const TARGETS_MAX = 10;

// The model Writing uses: the one chosen for Lumo (Settings → AI → Brain) when installed, else the first installed;
// null when Writing is off, there is no engine or no model.
export function writingModel({ on, available, models, chosen }) {
  if (!on || !available || !Array.isArray(models) || !models.length) return null;
  const ids = models.map((m) => m && m.id).filter(Boolean);
  return ids.includes(chosen) ? chosen : ids[0] || null;
}

// Writing is on whenever a model is installed, unless the person turned it off (settings.writing.on === false, kept
// for good: an explicit off is never turned back on, a new model included); choice: true, false or null (never chosen)
export const writingEnabled = (choice, hasModel) => !!hasModel && choice !== false;

// whether a field's menu shows ✨ AI: never in a password field, only with something selected and a model ready
export const offersWriting = ({ secure, selected, ready, allowed = true }) => !secure && allowed && !!ready && !!String(selected || '').trim();
export const tooLong = (text) => [...String(text || '')].length > WRITE_MAX_CHARS;

// "Translate to": the interface language, the spelling languages (en_US → en), then the common ones; at most 10
export function translateTargets(uiLang, spellTags = []) {
  const fromTag = (t) => { const l = String(t).split(/[_-]/)[0]; return l === 'zh' ? null : l; };
  const order = [uiLang, ...spellTags.map(fromTag), ...COMMON];
  return [...new Set(order.filter((l) => TRANSLATE_LANGS.includes(l)))].slice(0, TARGETS_MAX);
}

// The ✨ AI row of a text field's menu, with its submenu: the three actions, then "Translate to" and its languages.
// langName(code) names a language; disabled with a note when the selection is too long.
export function writingMenuItem({ tr, langName, targets, onAction, long = false }) {
  const label = `✨ ${tr('writing.menu.ai')}`;
  if (long) return { label, disabled: true, hint: tr('writing.menu.tooLong') };
  return {
    label,
    submenu: [
      ...WRITE_ACTIONS.map((a) => ({ label: tr(`writing.action.${a}`), onClick: () => onAction(a, null) })),
      '-',
      { header: tr('writing.menu.translate') },
      ...targets.map((l) => ({ label: langName(l), onClick: () => onAction('translate', l) })),
    ],
  };
}

// The model's text as plain text: no control characters (line breaks and tabs stay), Windows line ends made \n, a
// "<think>" block or the request's markers echoed back taken out, a code fence around the whole answer removed, and the
// spaces around it trimmed. Nothing in it is ever interpreted (it is shown in a Text and inserted as typed text).
export function cleanOutput(text) {
  let s = String(text || '').replace(/\r\n?/g, '\n');
  s = s.replace(/<think>[\s\S]*?<\/think>/g, '');
  s = s.replace(/^<<<(END-)?TEXT [0-9a-f]{16}>>>$/gm, '');
  s = s.replace(/[\u0000-\u0008\u000b-\u001f\u007f-\u009f‎‏‪-‮⁦-⁩]/g, '');
  const fence = /^\s*```[a-zA-Z]*\n([\s\S]*?)\n```\s*$/.exec(s);
  if (fence) s = fence[1];
  return s.trim();
}

// The panel's state: { phase: 'waiting' | 'writing' | 'done' | 'error', text, error }. Events: { type: 'peek', state,
// text } while lord-server streams, { type: 'take', result } at the end, { type: 'cancel' }.
export const writingStart = () => ({ phase: 'waiting', text: '', error: null });
export function writingStep(st, ev) {
  if (!ev || st.phase === 'done' || st.phase === 'error') return st;
  if (ev.type === 'peek') {
    if (ev.state === 'writing') return { ...st, phase: 'writing', text: cleanOutput(ev.text) };
    return st;
  }
  if (ev.type === 'take') {
    const r = ev.result;
    if (r && r.ok && typeof r.content === 'string') {
      const text = cleanOutput(r.content);
      return text ? { phase: 'done', text, error: null, truncated: !!r.truncated } : { phase: 'error', text: '', error: 'empty' };
    }
    return { phase: 'error', text: st.text, error: (r && r.error) || 'llm.failed' };
  }
  if (ev.type === 'cancel') return { phase: 'error', text: st.text, error: 'llm.cancelled' };
  return st;
}
export const canReplace = (st) => st.phase === 'done' && !!st.text;

// Replace only where the selection was, and only if it still holds the text sent: [a, b] in code points of value
export function stillThere(value, range, original) {
  if (!range) return false;
  return [...String(value || '')].slice(range[0], range[1]).join('') === original;
}
// a single-line field gets the answer on one line
export const forField = (text, multiline) => (multiline ? text : text.replace(/\s*\n\s*/g, ' '));
