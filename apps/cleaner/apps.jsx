// Apps: what is installed, largest first, and what removed LordOS apps left behind. A Linux app is removed by the
// package engine with lord.pkg.plan shown first and the admin helper's pkg-remove, as in the Store; a LordOS app with
// packages.remove. Leftovers are candidates of one rule, whole folders chosen by name, to the Trash.
import { View, Image, ScrollView, ConfirmDialog, admin, cleaner, packages, t as tr, formatBytes, locale, typeScale, spacing } from '@lordos/sdk';
import { useMemo, useState } from 'react';
import { Label, Glass, GlassButton, Check, Enter, PagedList, SelectAll, usePal } from './ui.jsx';
import { allIds } from './sections.jsx';
import { compareNames } from '../../sdk/collate.js';

const pkgReady = () => { try { return lord.pkg.available(); } catch (e) { return false; } };

function listApps() {
  const lang = locale.get().lang;
  const out = [];
  if (pkgReady()) for (const p of lord.pkg.installed({ lang })) {
    if (p.app && !p.essential && p.name !== 'lordos') out.push({ key: `deb:${p.name}`, name: p.app.name || p.name, icon: p.app.icon, detail: p.name, size: p.installedSize, pkg: p.name });
  }
  for (const a of packages.list().filter((x) => !x.packaged)) out.push({ key: `lpk:${a.id}`, name: a.name, icon: null, detail: a.id, size: 0, lpk: a.id });
  return out.sort((a, b) => b.size - a.size || compareNames(a.name, b.name));
}

function Row({ app, onRemove, canRemove }) {
  const p = usePal();
  return (
    <Glass style={{ padding: spacing.s, flexDirection: 'row', gap: spacing.m, alignItems: 'center' }}>
      <View style={{ width: 32, height: 32, borderRadius: 8, overflow: 'hidden', backgroundColor: p.track }}>
        {app.icon ? <Image src={app.icon} style={{ width: 32, height: 32 }} /> : null}
      </View>
      <View style={{ flex: 1, minWidth: 0 }}>
        <Label weight={600} lines={1}>{app.name}</Label>
        <Label dim size={typeScale.caption} lines={1}>{app.detail}</Label>
      </View>
      {app.size ? <Label size={typeScale.callout} weight={600}>{formatBytes(app.size)}</Label> : null}
      {canRemove ? <GlassButton small label={tr('cleaner.apps.remove')} onClick={() => onRemove(app)} /> : null}
    </Glass>
  );
}

function Leftovers({ scan, onFind, selected, setSelected }) {
  const rule = scan ? scan.rules.find((r) => r.id === 'app-leftovers') : null;
  return (
    <View style={{ gap: spacing.s }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m }}>
        <View style={{ flex: 1, minWidth: 0 }}>
          <Label weight={600}>{tr('cleaner.leftovers.title')}</Label>
          <Label dim size={typeScale.callout}>{tr('cleaner.rule.app-leftovers')}</Label>
        </View>
        {rule && rule.files ? <SelectAll count={selected.size} onAll={() => setSelected(new Set(allIds(scan.id, { rule: 'app-leftovers' })))} onNone={() => setSelected(new Set())} /> : null}
        <GlassButton small label={tr(scan ? 'cleaner.leftovers.again' : 'cleaner.leftovers.find')} onClick={onFind} />
      </View>
      {scan && !(rule && rule.files) ? <Label dim size={typeScale.callout}>{tr('cleaner.leftovers.none')}</Label> : null}
      {rule && rule.files ? (
        <Glass style={{ height: Math.min(200, 12 + rule.files * 34), padding: spacing.xs }}>
          <PagedList rowH={34} reset={scan.id} fetch={(o, l) => cleaner.results(scan.id, o, l, { rule: 'app-leftovers' })} render={(it) => (
            <View key={it.id} onClick={() => setSelected((old) => { const x = new Set(old); if (x.has(it.id)) x.delete(it.id); else x.add(it.id); return x; })}
              style={{ height: 34, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingHorizontal: spacing.s }}>
              <Check on={selected.has(it.id)} />
              <View style={{ flex: 1, minWidth: 0 }}><Label mono size={typeScale.callout} lines={1}>{it.path.split('/').pop()}</Label></View>
              <Label dim size={typeScale.callout}>{formatBytes(it.allocated)}</Label>
            </View>
          )} />
        </Glass>
      ) : null}
    </View>
  );
}

export function AppsView({ scan, onFind, selected, setSelected }) {
  const [gen, setGen] = useState(0);
  const [ask, setAsk] = useState(null);
  const [msg, setMsg] = useState('');
  const apps = useMemo(() => { try { return listApps(); } catch (e) { return []; } }, [gen]);
  const admins = useMemo(() => { try { return admin.available(); } catch (e) { return false; } }, []);
  const remove = (app) => {
    setMsg('');
    if (app.lpk) { setAsk({ app, items: [] }); return; }
    const p = lord.pkg.plan({ remove: [app.pkg] });
    if (!p.ok || p.items.some((i) => i.name === 'lordos')) { setMsg(tr('cleaner.apps.refused', { name: app.name })); return; }
    setAsk({ app, items: p.items.filter((i) => i.action === 'remove').map((i) => i.name) });
  };
  const run = () => {
    const { app } = ask;
    setAsk(null);
    const before = cleaner.status().free;
    const done = () => { cleaner.noteSystem('apps', Math.max(0, before)); setMsg(tr('cleaner.system.done', { name: app.name })); setGen((g) => g + 1); };
    if (app.lpk) { try { if (packages.remove(app.lpk)) done(); else setMsg(tr('cleaner.apps.failed', { name: app.name })); } catch (e) { setMsg(e.message); } return; }
    admin.run('pkg-remove', app.pkg).then(done).catch((e) => setMsg(e.message));
  };
  return (
    <Enter style={{ flex: 1, minHeight: 0, gap: spacing.s, paddingBottom: 96 }}>
      <Label size={32} weight={300}>{tr('cleaner.apps.title')}</Label>
      <Label dim size={typeScale.callout}>{tr('cleaner.apps.hint')}</Label>
      {msg ? <Label dim size={typeScale.callout}>{msg}</Label> : null}
      <ScrollView style={{ flex: 1 }} contentStyle={{ gap: spacing.s }}>
        <Leftovers scan={scan} onFind={onFind} selected={selected} setSelected={setSelected} />
        {!apps.length ? <Label dim>{tr('cleaner.apps.none')}</Label> : null}
        {apps.map((a) => <Row key={a.key} app={a} canRemove={!!a.lpk || admins} onRemove={remove} />)}
      </ScrollView>
      {ask ? <ConfirmDialog destructive title={tr('cleaner.apps.removeTitle', { name: ask.app.name })} detail={[ask.items.join(', '), tr(ask.app.lpk ? 'cleaner.apps.confirmLpk' : 'cleaner.system.confirm')].filter(Boolean).join('. ')}
        okLabel={tr('cleaner.apps.remove')} cancelLabel={tr('cleaner.cancel')} onOk={run} onCancel={() => setAsk(null)} /> : null}
    </Enter>
  );
}
