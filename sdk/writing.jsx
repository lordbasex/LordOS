// writing.jsx — "✨ AI" on selected text (docs/AI.md "Writing"): the shell's panel and its job. A text field hands the
// selection to openWriting's panel and gets the final text back only through onReplace, after the person's click;
// apps never reach the model (lord.writing exists only in the desktop's own context). The prompts are fixed in
// lord-server (server/src/llm/writing.c); the model's answer is plain text (writingmodel.js cleanOutput), shown as it
// streams in and inserted only on Replace. Off by default: Settings → AI → Writing (settings.writing.on).
import { useEffect, useRef, useState } from 'react';
import { View, Text } from '@lordos/runtime';
import { useTheme } from './theme.js';
import { t as tr, tOptional, locale } from './i18n.js';
import { settings } from './settings.js';
import { Overlay } from './layers.js';
import { Button, Spinner, ScrollView, takeTextFocus, releaseTextFocus, focusToken, restoreFocus, textFocusOwner } from './components.jsx';
import { writingModel, writingStart, writingStep, canReplace, translateTargets, writingMenuItem, tooLong, offersWriting } from './writingmodel.js';
import { spellLangs } from './spell.js';

const api = () => (typeof lord !== 'undefined' && lord.writing && typeof lord.writing.start === 'function' ? lord.writing : null);
const engine = () => (typeof lord !== 'undefined' && lord.sentinela) || null;

export const writingOn = () => !!(settings.get().writing || {}).on;
export function setWritingOn(on) { settings.set((s) => ({ ...s, writing: { ...(s.writing || {}), on: !!on } })); }

// the model to write with, or null (Writing off, no engine, no model); checked when a menu opens
export function writingReady() {
  if (!api() || !writingOn()) return null;
  const e = engine();
  let available = false, models = [];
  try { available = !!e?.available(); models = e?.models() || []; } catch (err) { available = false; }
  return writingModel({ on: true, available, models, chosen: (settings.get().sentinela || {}).model });
}

// The ✨ AI row for a field's menu, or null when it isn't offered. open(action, target) opens the panel.
export function writingMenuFor({ secure, selected, allowed = true, open }) {
  const model = offersWriting({ secure, selected, allowed, ready: true }) ? writingReady() : null;
  if (!model) return null;
  const langName = (c) => tOptional(`languages.${c}`) || c;
  return writingMenuItem({ tr, langName, targets: translateTargets(locale.get().lang, spellLangs()), long: tooLong(selected),
    onAction: (action, target) => open({ action, target, model }) });
}

// The job: lord.writing.start, then its text as it streams (peek) and the end (take); onState(state) each time.
// Returns a function that cancels it.
function runJob({ model, action, target, text }, onState) {
  const a = api();
  let st = writingStart(), id = 0, stop = false;
  try { id = a.start(model, action, target || null, text); } catch (e) { onState(writingStep(st, { type: 'take', result: { ok: false, error: e.code || 'llm.failed' } })); return () => {}; }
  const until = Date.now() + 200000;
  const tick = () => {
    if (stop) return;
    let r = null;
    try { r = a.take(id); } catch (e) { r = { ok: false, error: 'llm.failed' }; }
    if (r) { st = writingStep(st, { type: 'take', result: r }); onState(st); return; }
    if (Date.now() > until) { try { a.cancel(id); } catch (e) { /* gone */ } st = writingStep(st, { type: 'take', result: { ok: false, error: 'llm.timeout' } }); onState(st); return; }
    let p = null;
    try { p = a.peek(id); } catch (e) { p = null; }
    if (p) { const n = writingStep(st, { type: 'peek', state: p.state, text: p.text }); if (n !== st) { st = n; onState(st); } }
    setTimeout(tick, 120);
  };
  setTimeout(tick, 60);
  return () => { stop = true; try { a.cancel(id); } catch (e) { /* gone */ } };
}

const TITLE = { fix: 'writing.action.fix', improve: 'writing.action.improve', shorter: 'writing.action.shorter' };

// The panel, at { x, y } of the field's box (anchor), above the windows; bounds as the field's menu has them.
// job: { action, target, text, model }; onReplace(text) → false when the field's text changed meanwhile.
export function WritingPanel({ anchor, at, bounds, job, onReplace, onClose }) {
  const t = useTheme();
  const [st, setSt] = useState(writingStart);
  const [note, setNote] = useState(null);           // 'copied' | 'changed'
  const cancel = useRef(null);
  const view = useRef({});
  view.current = { st, onClose, onReplace };
  useEffect(() => {
    cancel.current = runJob(job, setSt);
    return () => cancel.current?.();
  }, []);
  const copy = () => { const s = view.current.st; if (s.text) { lord.clipboard.set(s.text, { origin: 'lordos' }); setNote('copied'); } };
  const replace = () => {
    const s = view.current.st;
    if (!canReplace(s)) return;
    if (view.current.onReplace(s.text) === false) { setNote('changed'); return; }
    view.current.onClose();
  };
  const discard = () => { cancel.current?.(); view.current.onClose(); };
  // the keys while it is open: Esc discards, Enter replaces (as its buttons), the field gets them back after
  useEffect(() => {
    const prev = focusToken();
    const tok = {};
    takeTextFocus(tok, (e) => {
      if (e.key === 'Escape') discard();
      else if (e.key === 'Enter' && !e.shift) replace();
      else if ((e.ctrl || e.meta) && e.base === 'c') copy();
    });
    return () => { const mine = textFocusOwner() === tok; releaseTextFocus(tok); if (mine) restoreFocus(prev); };
  }, []);
  const W = 400;
  const b = bounds || { w: 4000, h: 4000 };
  const left = Math.max(4, Math.min(at.x, b.w - W - 4)), top = Math.max(4, Math.min(at.y + 6, b.h - 300));
  const title = job.action === 'translate'
    ? tr('writing.action.translateTo', { language: tOptional(`languages.${job.target}`) || job.target }) : tr(TITLE[job.action]);
  const busy = st.phase === 'waiting' || st.phase === 'writing';
  const errText = st.phase === 'error' ? (st.error === 'llm.timeout' ? tr('writing.panel.timeout') : tr('writing.panel.failed')) : '';
  return (
    <Overlay anchor={anchor}>
      <View keepFocus style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, pointerEvents: 'box-none' }}>
        <View onMouseDown={(e) => e?.stopPropagation?.()} style={{ position: 'absolute', left, top, width: W, padding: 12, gap: 10, borderRadius: 12,
          backgroundColor: t.mode === 'dark' ? '#2c2c31' : '#ffffff', border: `1px solid ${t.separator}`, boxShadow: '0 12px 34px rgba(0,0,0,0.35)' }}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
            <Text style={{ fontSize: 15 }}>✨</Text>
            <Text style={{ flex: 1, fontSize: 13, fontWeight: 600, color: t.text, textAlign: 'start' }}>{title}</Text>
            {busy ? <Spinner size={13} /> : null}
          </View>
          <View style={{ minHeight: 60, maxHeight: 220, borderRadius: 8, backgroundColor: t.inputBg, border: `1px solid ${t.separator}`, overflow: 'hidden' }}>
            <ScrollView style={{ maxHeight: 218 }} contentStyle={{ padding: 9 }}>
              {st.text ? <Text style={{ fontSize: 13, lineHeight: 19, color: t.text, textAlign: 'start', whiteSpace: 'normal' }}>{st.text}</Text>
                : <Text style={{ fontSize: 12, color: t.textDim, textAlign: 'start' }}>{st.phase === 'error' ? errText : tr('writing.panel.waiting')}</Text>}
            </ScrollView>
          </View>
          {st.phase === 'error' && st.text ? <Text style={{ fontSize: 11, color: '#ff453a', textAlign: 'start' }}>{errText}</Text> : null}
          {note ? <Text style={{ fontSize: 11, color: note === 'changed' ? '#ff9f0a' : t.textDim, textAlign: 'start' }}>{tr(`writing.panel.${note}`)}</Text> : null}
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
            <Text style={{ flex: 1, fontSize: 11, color: t.textDim, textAlign: 'start' }}>{tr('writing.panel.local')}</Text>
            <Button small label={tr('writing.panel.discard')} onClick={discard} />
            <Button small label={tr('writing.panel.copy')} onClick={copy} disabled={!st.text || busy} />
            <Button small primary label={tr('writing.panel.replace')} onClick={replace} disabled={!canReplace(st)} />
          </View>
        </View>
      </View>
    </Overlay>
  );
}
