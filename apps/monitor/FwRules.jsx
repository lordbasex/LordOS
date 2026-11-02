// FwRules.jsx — Settings → Privacy → "Network rules by app" (registered with
// registerPrivacySection after "App network access", see docs/FIREWALL.md, "Rules by app").
// For each app:
//   - its default: allow everything, or block everything except its rules (the {out, any} rule,
//     the same one the Monitor's switch writes);
//   - its rules: allow or block a domain (exact, or *.example.com with every subdomain), an IP
//     address or a CIDR network, with an optional port or range and protocol. Each row says when
//     the rule last decided a connection (lord-netperm's notices, seen by this session);
//   - add (a sheet that checks what is typed), edit and delete. There is no priority to reorder:
//     the engine picks the most specific match (rules.c, fw_decide), so the list is shown in that
//     order and the footer explains it.
// The rules are saved by lord-server (lord.fw); this screen writes no files.
import {
  View, Text, Button, Segmented, ScrollView, GroupBox, useTheme, useLocale, useStore, useLauncherApps, firewall, firewallState, fwRule,
  spacing, typeScale, formatDate, formatTime, direction, netModel,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { RuleSheet, destKind, RED, GREEN } from './RuleSheet.jsx';

const { sortByPrecedence, isDefaultRule } = fwRule;

// Firewall id of a launcher app: the built-in ones run inside LordOS; isolated ones have a scope.
const fwId = (a) => (a.kind === 'linux' ? a.id : a.kind === 'package' || a.isolated ? `lpk:${a.id}` : 'lordos');

function useAppIndex() {
  const list = useLauncherApps();
  const byId = new Map();
  for (const a of list) {
    const id = fwId(a);
    if (id !== 'lordos' && !byId.has(id)) byId.set(id, a);
  }
  return byId;
}

// (0.8.5) LordOS's own components with their own scope take rules too (Mail, Store, Printers, Terminal, Blocklists)
const { COMPONENTS } = netModel;
const COMPONENT_IDS = Object.keys(COMPONENTS).filter((id) => id.startsWith('lordos:') && !COMPONENTS[id].system);
export const appLabelOf = (tr, apps, id) => (id === 'lordos' ? tr('monitor.lordos') : COMPONENTS[id] ? tr(COMPONENTS[id].label) : apps.get(id)?.name || id.replace(/^(desktop|lpk):/, ''));

function AppIcon({ a, size = 24 }) {
  const t = useTheme();
  const Icon = a?.Icon;
  return Icon ? <Icon size={size} /> : <View style={{ width: size, height: size, borderRadius: 6, backgroundColor: t.control }} />;
}

// "2 min ago", "today 14:05", "18/10/2026 14:05", or "not seen yet".
export function lastMatchText(tr, secs) {
  if (!secs) return tr('fwrules.lastMatch.never');
  const now = Date.now() / 1000, d = Math.max(0, now - secs);
  if (d < 60) return tr('fwrules.lastMatch.now');
  if (d < 3600) return tr('fwrules.lastMatch.minutes', { count: Math.floor(d / 60) });
  const when = new Date(secs * 1000), today = new Date();
  const time = formatTime(when);
  return when.toDateString() === today.toDateString() ? tr('fwrules.lastMatch.today', { time }) : tr('fwrules.lastMatch.date', { date: formatDate(when), time });
}

function ActionPill({ action }) {
  const { t: tr } = useLocale();
  const allow = action === 'allow';
  return (
    <View style={{ width: 52, height: 18, borderRadius: 9, alignItems: 'center', justifyContent: 'center', backgroundColor: allow ? 'rgba(48,209,88,0.18)' : 'rgba(255,69,58,0.18)' }}>
      <Text style={{ fontSize: 10, fontWeight: 600, color: allow ? GREEN : RED }}>{tr(allow ? 'fwrules.allow' : 'fwrules.block')}</Text>
    </View>
  );
}

function RuleRow({ r, onEdit, onDelete, first }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const detail = [destKind(tr, r.dst), r.port ? tr('fwrules.portValue', { port: r.port }) : tr('fwrules.anyPort'), r.proto ? r.proto.toUpperCase() : tr('fwrules.anyProto')];
  if (r.dir === 'in') detail.push(tr('fwrules.incoming'));
  return (
    <View style={{ minHeight: 44, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingVertical: 6,
      borderTopWidth: first ? 0 : 1, borderTopColor: t.separator }}>
      <ActionPill action={r.action} />
      <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
        <Text style={{ fontSize: typeScale.body, color: t.text, fontFamily: 'monospace', whiteSpace: 'nowrap', textOverflow: 'ellipsis', overflow: 'hidden', direction: 'ltr', textAlign: 'start' }}>{r.dst}</Text>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap', textOverflow: 'ellipsis', overflow: 'hidden', textAlign: 'start' }}>
          {`${detail.join(' · ')} — ${lastMatchText(tr, r.lastMatch)}`}
        </Text>
      </View>
      <Button small plain label={tr('fwrules.edit')} onClick={onEdit} />
      <Button small plain destructive label={tr('fwrules.delete')} onClick={onDelete} />
    </View>
  );
}

// One app: header (icon, name, summary, chevron) and, open, its default, rules and the sheet.
function AppRules({ id, apps, open, onToggle }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [sheet, setSheet] = useState(null);             // null | { editId?, initial }
  const [confirm, setConfirm] = useState(0);            // the id waiting for a second click on Delete
  const all = firewall.rules(id);
  const def = all.find(isDefaultRule);
  const rules = sortByPrecedence(all.filter((r) => !isDefaultRule(r)));
  const label = appLabelOf(tr, apps, id);
  const blocked = def?.action === 'deny';
  const can = firewall.enforceable(id);
  const summary = `${tr('fwrules.count', { count: rules.length })} · ${tr(blocked ? 'fwrules.default.denyShort' : 'fwrules.default.allowShort')}`;
  return (
    <View style={{ borderTopWidth: 0 }}>
      <View onClick={onToggle} style={{ minHeight: 44, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingVertical: 6 }}>
        <AppIcon a={apps.get(id)} />
        <View style={{ flex: 1, minWidth: 0, gap: 1 }}>
          <Text style={{ fontSize: typeScale.body, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{label}</Text>
          <Text style={{ fontSize: typeScale.caption, color: blocked ? RED : t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{summary}</Text>
        </View>
        <Text style={{ fontSize: 11, color: t.textDim }}>{open ? '▾' : direction() === 'rtl' ? '◂' : '▸'}</Text>
      </View>
      {open && (
        <View style={{ gap: spacing.m, paddingBottom: spacing.m, paddingInlineStart: 32 }}>
          {!can ? <Text style={{ fontSize: typeScale.caption, color: '#ff9f0a', lineHeight: 1.35, textAlign: 'start' }}>{tr('fwrules.notEnforceable')}</Text> : null}
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m }}>
            <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
              <Text style={{ fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{tr('fwrules.default')}</Text>
              <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35, textAlign: 'start' }}>{tr(blocked ? 'fwrules.default.denyHint' : 'fwrules.default.allowHint')}</Text>
            </View>
            <Segmented small value={blocked ? 'deny' : 'allow'} onChange={(v) => firewall.setAppRule(id, v === 'deny' ? 'deny' : null)}
              options={[['allow', tr('fwrules.default.allow')], ['deny', tr('fwrules.default.deny')]]} />
          </View>
          <View style={{ borderRadius: 8, border: `1px solid ${t.separator}`, paddingHorizontal: spacing.m }}>
            {rules.map((r, i) => (
              confirm === r.id ? (
                <View key={r.id} style={{ minHeight: 44, flexDirection: 'row', alignItems: 'center', gap: spacing.s, borderTopWidth: i ? 1 : 0, borderTopColor: t.separator }}>
                  <Text style={{ flex: 1, fontSize: typeScale.callout, color: t.text, textAlign: 'start' }}>{tr('fwrules.confirmDelete', { dst: r.dst })}</Text>
                  <Button small label={tr('fwrules.cancel')} onClick={() => setConfirm(0)} />
                  <Button small destructive label={tr('fwrules.delete')} onClick={() => { setConfirm(0); firewall.removeRule(r.id); }} />
                </View>
              ) : (
                <RuleRow key={r.id} r={r} first={i === 0} onEdit={() => setSheet({ editId: r.id, initial: r })} onDelete={() => setConfirm(r.id)} />
              )
            ))}
            {!rules.length && <Text style={{ fontSize: typeScale.callout, color: t.textDim, paddingVertical: 12, textAlign: 'start' }}>{tr('fwrules.empty')}</Text>}
          </View>
          {sheet ? (
            <RuleSheet key={sheet.editId || 'new'} app={id} appLabel={label} editId={sheet.editId} initial={sheet.initial}
              onDone={() => setSheet(null)} />
          ) : (
            <View style={{ flexDirection: 'row', justifyContent: 'flex-end' }}>
              <Button small label={tr('fwrules.add')} onClick={() => setSheet({ initial: { action: blocked ? 'allow' : 'deny' } })} />
            </View>
          )}
        </View>
      )}
    </View>
  );
}

// Drop-down list to pick another app.
function AddApp({ apps, exclude, onPick }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [open, setOpen] = useState(false);
  const choices = ['lordos', ...COMPONENT_IDS, ...apps.keys()].filter((id) => !exclude.has(id));
  return (
    <View style={{ gap: spacing.s, paddingVertical: spacing.s }}>
      <View style={{ flexDirection: 'row' }}>
        <Button small label={open ? tr('fwrules.close') : tr('fwrules.addApp')} onClick={() => setOpen(!open)} />
      </View>
      {open && (
        <View style={{ height: 180, borderRadius: 8, border: `1px solid ${t.separator}`, backgroundColor: t.mode === 'dark' ? 'rgba(0,0,0,0.12)' : '#ffffff' }}>
          <ScrollView style={{ flex: 1, padding: 4 }}>
            {choices.map((id) => (
              <View key={id} onClick={() => { setOpen(false); onPick(id); }} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 8, paddingVertical: 5, borderRadius: 5 }}>
                <AppIcon a={apps.get(id)} size={20} />
                <Text style={{ flex: 1, fontSize: typeScale.body, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{appLabelOf(tr, apps, id)}</Text>
              </View>
            ))}
            {!choices.length && <Text style={{ fontSize: 12, color: t.textDim, padding: 8 }}>{tr('fwrules.noMoreApps')}</Text>}
          </ScrollView>
        </View>
      )}
    </View>
  );
}

export function FirewallRulesPrivacy() {
  const t = useTheme();
  const { t: tr } = useLocale();
  useStore(firewallState);                              // redrawn when a rule changes
  const focus = useStore(firewall.focus);
  const apps = useAppIndex();
  const [extra, setExtra] = useState([]);                // apps added here that have no rule yet
  const [open, setOpen] = useState(() => new Set());
  // "last matched" moves on its own: look again every few seconds while the page is shown
  const [, tick] = useState(0);
  useEffect(() => { const id = setInterval(() => tick((x) => x + 1), 5000); return () => clearInterval(id); }, []);
  // the Monitor's "Rules for this app" opens that app
  useEffect(() => {
    if (!focus) return;
    setExtra((l) => (l.includes(focus) ? l : [...l, focus]));
    setOpen((s) => new Set([...s, focus]));
    firewall.focus.set(() => null);
  }, [focus]);
  if (!firewall.available()) return null;
  const withRules = firewall.ruleApps();
  const ids = [...new Set([...withRules, ...extra])].sort((a, b) => appLabelOf(tr, apps, a).localeCompare(appLabelOf(tr, apps, b)));
  const toggle = (id) => setOpen((s) => { const n = new Set(s); if (n.has(id)) n.delete(id); else n.add(id); return n; });
  const foot = (text) => <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.4, marginHorizontal: spacing.xs, textAlign: 'start' }}>{text}</Text>;
  return (
    <View style={{ gap: 6 }}>
      <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, paddingHorizontal: spacing.xs, textAlign: 'start' }}>{tr('fwrules.title')}</Text>
      <GroupBox>
        {ids.map((id, i) => (
          <View key={id} style={{ borderTopWidth: i ? 1 : 0, borderTopColor: t.separator }}>
            <AppRules id={id} apps={apps} open={open.has(id)} onToggle={() => toggle(id)} />
          </View>
        ))}
        {!ids.length && <Text style={{ fontSize: typeScale.body, color: t.textDim, paddingVertical: 9, textAlign: 'start' }}>{tr('fwrules.noApps')}</Text>}
        <AddApp apps={apps} exclude={new Set(ids)} onPick={(id) => { setExtra((l) => [...l, id]); setOpen((s) => new Set([...s, id])); }} />
      </GroupBox>
      {foot(tr('fwrules.precedence'))}
      {foot(tr('fwrules.dnsNote'))}
    </View>
  );
}
