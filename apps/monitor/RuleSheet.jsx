// RuleSheet.jsx — the sheet that adds or edits one firewall rule of an app: allow or block, a
// destination (an exact domain, a wildcard *.example.com, an IP address or a CIDR network), an
// optional port or range and an optional protocol. Used by Settings → Privacy → "Network rules by
// app" (inline, under the app's rules) and by the Monitor's Network tab ("Allow / Block this
// domain", over the window). What the user types is checked as they type with sdk/fwrule.js, the
// same rules as the C store; lord.fw.addRule / updateRule check it again in C.
// Layout per docs/DESIGN.md "Dialogs and sheets": 20 px padding, a 15/600 title, 13 px body,
// the buttons at the end with 12 px above them and one primary action.
import { View, Text, Button, Segmented, TextInput, useTheme, useLocale, firewall, fwRule, spacing, typeScale, formatNumber } from '@lordos/sdk';
import { useState } from 'react';

const { checkRule, parseDest } = fwRule;

export const RED = '#ff453a', GREEN = '#30d158';

// What a destination covers, in words (under the field).
export function destMeaning(tr, dst) {
  const d = parseDest(dst);
  if (!d.ok) return null;
  switch (d.kind) {
    case 'domain': return tr('fwrules.meaning.domain', { name: d.dst });
    case 'wildcard': return tr('fwrules.meaning.wildcard', { name: d.domain });
    case 'address': return tr('fwrules.meaning.address');
    case 'network': {
      const bits = (d.fam === 4 ? 32 : 128) - d.prefix;
      return bits <= 32 ? tr('fwrules.meaning.network', { count: formatNumber(2 ** bits, 0) }) : tr('fwrules.meaning.networkLarge');
    }
    case 'lan': return tr('fwrules.meaning.lan');
    case 'internet': return tr('fwrules.meaning.internet');
    default: return tr('fwrules.meaning.any');
  }
}

// Short label of a destination's kind (the rules list).
export function destKind(tr, dst) {
  const d = parseDest(dst);
  return d.ok ? tr(`fwrules.kind.${d.kind}`) : '';
}

function Field({ label, children, hint, hintColor }) {
  const t = useTheme();
  return (
    <View style={{ gap: 4 }}>
      <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{label}</Text>
      {children}
      {hint ? <Text style={{ fontSize: typeScale.caption, color: hintColor || t.textDim, lineHeight: 1.35, textAlign: 'start' }}>{hint}</Text> : null}
    </View>
  );
}

// initial: { dst, port, proto, action, dir } (dir stays as it is); editId: the rule being edited.
// suggestions: other destinations to offer as chips (the Monitor offers the exact name and its
// *.parent). onDone(id | null): saved (the new id) or cancelled.
export function RuleSheet({ app, appLabel, initial = {}, editId, suggestions = [], onDone, overlay, note }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [f, setF] = useState(() => ({ dst: initial.dst || '', port: initial.port || '', proto: initial.proto || '', action: initial.action || 'deny', dir: initial.dir || 'out' }));
  const [failed, setFailed] = useState(false);
  const put = (p) => { setFailed(false); setF((o) => ({ ...o, ...p })); };
  const res = checkRule(f);
  const dstErr = !res.ok && res.field === 'dst' && f.dst.trim() ? tr(`fwrules.err.${res.error}`) : null;
  const portErr = !res.ok && res.field === 'port' ? tr('fwrules.err.port') : null;
  const meaning = res.ok ? destMeaning(tr, res.rule.dst) : null;
  const save = () => {
    if (!res.ok) return;
    const rule = { app, ...res.rule };
    const id = editId ? firewall.updateRule(editId, res.rule) : firewall.addRule(rule);
    if (id) onDone(id); else setFailed(true);
  };
  const title = editId ? tr('fwrules.sheet.editTitle') : tr(f.action === 'allow' ? 'fwrules.sheet.allowTitle' : 'fwrules.sheet.blockTitle');
  const box = (
    // handsDeny: a firewall rule is the person's own decision (docs/HANDS.md; lord.fw refuses an agent natively too)
    <View handsDeny style={{ gap: spacing.l, padding: spacing.xl, borderRadius: 12, width: overlay ? 440 : undefined,
      backgroundColor: overlay ? (t.mode === 'dark' ? '#2a2a2f' : '#ffffff') : t.mode === 'dark' ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.025)',
      border: `1px solid ${t.separator}`, boxShadow: overlay ? '0 18px 50px rgba(0,0,0,0.35)' : 'none' }}>
      <View style={{ gap: 3 }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{title}</Text>
        {appLabel ? <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('fwrules.sheet.forApp', { app: appLabel })}</Text> : null}
      </View>
      <Field label={tr('fwrules.field.action')}>
        <View style={{ flexDirection: 'row' }}>
          <Segmented small value={f.action} onChange={(action) => put({ action })}
            options={[['allow', tr('fwrules.allow')], ['deny', tr('fwrules.block')]]} />
        </View>
      </Field>
      <Field label={tr('fwrules.field.dst')} hint={dstErr || meaning || tr('fwrules.field.dstHint')} hintColor={dstErr ? RED : null}>
        <TextInput value={f.dst} autoFocus={!initial.dst} placeholder={tr('fwrules.field.dstPlaceholder')} onChange={(dst) => put({ dst })} onSubmit={save}
          onCancel={() => onDone(null)} style={{ direction: 'ltr' }} />
        {suggestions.length > 1 ? (
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6 }}>
            {suggestions.map((s) => (
              <View key={s} onClick={() => put({ dst: s })} style={{ height: 22, paddingHorizontal: 9, borderRadius: 11, justifyContent: 'center',
                backgroundColor: f.dst === s ? t.accent : t.control }}>
                <Text style={{ fontSize: typeScale.caption, color: f.dst === s ? '#fff' : t.text, fontFamily: 'monospace', direction: 'ltr' }}>{s}</Text>
              </View>
            ))}
          </View>
        ) : null}
      </Field>
      <View style={{ flexDirection: 'row', gap: spacing.l, flexWrap: 'wrap' }}>
        <Field label={tr('fwrules.field.port')} hint={portErr || tr('fwrules.field.portHint')} hintColor={portErr ? RED : null}>
          <TextInput spellCheck={false} value={f.port} placeholder={tr('fwrules.any')} onChange={(port) => put({ port })} onSubmit={save} onCancel={() => onDone(null)} style={{ width: 130, direction: 'ltr' }} />
        </Field>
        <Field label={tr('fwrules.field.proto')}>
          <View style={{ flexDirection: 'row', height: 28, alignItems: 'center' }}>
            <Segmented small value={f.proto} onChange={(proto) => put({ proto })} options={[['', tr('fwrules.any')], ['tcp', 'TCP'], ['udp', 'UDP']]} />
          </View>
        </Field>
      </View>
      {note ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{note}</Text> : null}
      {failed ? <Text style={{ fontSize: typeScale.caption, color: RED, textAlign: 'start' }}>{tr('fwrules.err.save')}</Text> : null}
      <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s, marginTop: spacing.xs }}>
        <Button label={tr('fwrules.cancel')} onClick={() => onDone(null)} />
        <Button primary label={editId ? tr('fwrules.save') : tr('fwrules.addRule')} disabled={!res.ok} onClick={save} />
      </View>
    </View>
  );
  if (!overlay) return box;
  return (
    <View handsDeny style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.28)' }}>
      {box}
    </View>
  );
}
