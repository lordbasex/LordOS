// The library (alpha): the machines in ~/Machines with their state, what this computer still needs, and the
// actions New, Start or Open, Stop, Delete. The machine's own window does the rest (vmwindow.jsx).
import {
  View, Text, ScrollView, Button, useTheme, useAppWindow, useSystemInfo, admin, fs, wm, windows, statusText,
  Toolbar, ToolbarSpacer, MenuButton, EmptyState, ConfirmDialog, spacing, typeScale,
  t as tr,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { stateView, actionsFor, missingFor } from './model.js';
import { NewMachine } from './newsheet.jsx';
import { AlphaBadge, StateDot, has, gb } from './ui.jsx';

// opens (or brings forward) the machine's window; a new one gets the guest's usual size and the middle of the screen
export function openMachine(m) {
  const instance = `vm-${m.id || m.folder}`;
  const id = `machines#${instance}`;
  const fresh = !windows.get().some((w) => w.id === id);
  wm.open('machines', { instance, args: { machine: m.dir, name: m.name } });
  if (fresh) { wm.resize(id, 1024, 768 + 36); wm.center(id); }
  return id;
}

function Requirements({ host, onChanged }) {
  const t = useTheme();
  const info = useSystemInfo(60000);
  const [busy, setBusy] = useState('');
  const [msg, setMsg] = useState(null);
  const missing = missingFor(host, [host.arch]);
  const canAdmin = admin.available() && info.admin;
  const install = async () => {
    setBusy('install'); setMsg(null);
    const go = () => admin.run('vm-enable', host.arch, (line) => setMsg({ ok: true, text: statusText(line) }));
    try {
      try { await go(); } catch (e) {
        if (e.code === 'pkg.busy') throw e;
        await admin.run('pkg-update', '');           // an index older than the packages: read it again, once
        await go();
      }
      setMsg({ ok: true, text: tr('machines.req.installed') });
    } catch (e) { setMsg({ ok: false, text: e.message }); }
    setBusy('');
    onChanged();
  };
  const allow = async () => {
    setBusy('kvm'); setMsg(null);
    try { const r = await admin.run('vm-kvm', ''); setMsg({ ok: true, text: statusText(r) }); } catch (e) { setMsg({ ok: false, text: e.message }); }
    setBusy('');
    onChanged();
  };
  const rows = [];
  if (missing.length) rows.push(
    <View key="p" style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m }}>
      <View style={{ flex: 1, gap: 2 }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('machines.req.title')}</Text>
        <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start', lineHeight: 1.35 }}>{tr('machines.req.packages', { list: missing.join(', ') })}</Text>
      </View>
      <Button primary small label={busy === 'install' ? tr('machines.req.installing') : tr('machines.req.install')} disabled={!!busy || !canAdmin} onClick={install} />
    </View>);
  if (host.kvmDevice && !host.kvm) rows.push(
    <View key="k" style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m }}>
      <View style={{ flex: 1, gap: 2 }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('machines.req.kvmTitle')}</Text>
        <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start', lineHeight: 1.35 }}>{tr('machines.req.kvm')}</Text>
      </View>
      <Button small label={busy === 'kvm' ? tr('machines.req.allowing') : tr('machines.req.kvmAllow')} disabled={!!busy || !canAdmin} onClick={allow} />
    </View>);
  if (!host.kvmDevice) rows.push(<Text key="n" style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('machines.req.noKvm')}</Text>);
  if (!rows.length && !msg) return null;
  return (
    <View style={{ marginHorizontal: spacing.m, marginTop: spacing.m, padding: spacing.m, gap: spacing.s, borderRadius: 10, border: `1px solid ${t.separator}`,
      backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.025)' }}>
      {rows}
      {rows.length && !canAdmin ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr('machines.req.notAdmin')}</Text> : null}
      {msg ? <Text style={{ fontSize: typeScale.callout, color: msg.ok ? t.textDim : '#ff453a', textAlign: 'start' }}>{msg.text}</Text> : null}
    </View>
  );
}

function Row({ m, selected, onSelect, onDelete, onLog, onEject }) {
  const t = useTheme();
  const v = stateView(m.state, m.reason);
  const a = actionsFor(m.state);
  const emulated = m.accel === 'tcg';
  const stop = (how) => { lord.vm.stop(m.dir, how); };
  const facts = m.error ? tr(`machines.bad.${m.error}`) : [m.arch, emulated ? tr('machines.emulated') : null, tr('machines.vcpus', { n: m.vcpus }), gb(m.memoryMb), tr('machines.disk', { n: m.diskGb }), m.iso ? tr('machines.isoAttached') : null].filter(Boolean).join(' · ');
  return (
    <View onClick={onSelect} style={{ minHeight: 56, paddingHorizontal: spacing.m, paddingVertical: spacing.s, flexDirection: 'row', alignItems: 'center', gap: spacing.m,
      borderRadius: 8, backgroundColor: selected ? t.hover : 'transparent' }}>
      <View style={{ width: 36, height: 28, borderRadius: 6, alignItems: 'center', justifyContent: 'center', backgroundColor: '#0c1220' }}>
        <View style={{ width: 16, height: 11, borderRadius: 3, backgroundColor: v.tone === 'run' ? '#5ad1a8' : '#3a4a63' }} />
      </View>
      <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{m.name}</Text>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{facts}</Text>
        {v.why ? <Text onClick={onLog} style={{ fontSize: typeScale.caption, color: '#ff453a', textAlign: 'start' }}>{`${tr(`machines.why.${v.why}`)} · ${tr('machines.showLog')}`}</Text> : null}
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, minWidth: 96 }}>
        <StateDot tone={v.tone} />
        <Text style={{ fontSize: typeScale.callout, color: t.textDim }}>{tr(`machines.state.${v.key}`)}</Text>
      </View>
      {a.open ? <Button small primary label={tr('machines.open')} onClick={() => openMachine(m)} /> : null}
      {a.start ? <Button small primary label={tr('machines.start')} disabled={!!m.error} onClick={() => openMachine(m)} /> : null}
      {a.poweroff ? <MenuButton small icon={null} label={tr('machines.stop')} title={tr('machines.stop')} items={[
        { label: tr('machines.cmd.shutdown'), disabled: !a.shutdown, onClick: () => stop('shutdown') },
        { label: tr('machines.cmd.poweroff'), destructive: true, onClick: () => stop('poweroff') }]} /> : null}
      {a.remove && m.iso ? <Button small plain label={tr('machines.eject')} title={m.iso} onClick={onEject} /> : null}
      {a.remove ? <Button small plain label={tr('machines.delete')} onClick={onDelete} /> : null}
    </View>
  );
}

export function Library() {
  const t = useTheme();
  const { id: winId } = useAppWindow();
  const [host, setHost] = useState(() => (has() ? lord.vm.host() : null));
  const [list, setList] = useState(() => (has() ? lord.vm.list() : []));
  const [sel, setSel] = useState(null);
  const [sheet, setSheet] = useState(false);
  const [del, setDel] = useState(null);
  const [log, setLog] = useState(null);
  const [error, setError] = useState('');
  const refresh = () => { if (has()) { setList(lord.vm.list()); setHost(lord.vm.host()); } };
  useEffect(() => {
    wm.setTitle(winId, tr('machines.title'));
    const timer = setInterval(() => { if (has()) setList(lord.vm.list()); }, 1500);
    return () => clearInterval(timer);
  }, []);
  if (!host) return <EmptyState icon="▣" title={tr('machines.title')} hint={tr('machines.noBackend')} />;
  const sorted = [...list].sort((a, b) => a.name.localeCompare(b.name));
  const remove = (m) => {
    setDel(null);
    try { fs.trash(m.dir); setError(''); } catch (e) { setError(tr('machines.deleteFailed', { name: m.name })); }
    refresh();
  };
  return (
    <View style={{ flex: 1, backgroundColor: t.windowBg }}>
      <Toolbar>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text }}>{tr('machines.title')}</Text>
        <AlphaBadge />
        <ToolbarSpacer />
        <Button small primary label={tr('machines.new')} onClick={() => setSheet(true)} />
      </Toolbar>
      <Requirements host={host} onChanged={refresh} />
      {error ? <Text style={{ fontSize: typeScale.callout, color: '#ff453a', paddingHorizontal: spacing.l, paddingTop: spacing.s, textAlign: 'start' }}>{error}</Text> : null}
      {!sorted.length ? (
        <EmptyState icon="▣" title={tr('machines.empty')} hint={tr('machines.emptyHint')} action={<Button primary label={tr('machines.newMachine')} onClick={() => setSheet(true)} />} />
      ) : (
        <ScrollView style={{ flex: 1 }} contentStyle={{ padding: spacing.s, gap: 2 }}>
          {sorted.map((m) => (
            <Row key={m.dir} m={m} selected={sel === m.dir} onSelect={() => setSel(m.dir)} onDelete={() => setDel(m)}
              onLog={() => setLog({ name: m.name, text: lord.vm.log(m.dir) })}
              onEject={() => { setError(lord.vm.eject(m.dir) ? '' : tr('machines.ejectFailed', { name: m.name })); refresh(); }} />
          ))}
        </ScrollView>
      )}
      <View style={{ height: 26, flexDirection: 'row', alignItems: 'center', paddingHorizontal: spacing.m, borderTopWidth: 1, borderColor: t.separator }}>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>
          {tr('machines.status', { running: list.filter((m) => stateView(m.state).active).length, total: list.length, cpus: host.cpus, ram: gb(host.ramMb) })}
        </Text>
      </View>
      {sheet ? <NewMachine host={host} names={list.map((m) => m.name)} onClose={() => setSheet(false)} onHostChanged={refresh}
        onCreated={(m) => { setSheet(false); refresh(); if (m) openMachine(m); }} /> : null}
      {del ? <ConfirmDialog title={tr('machines.deleteTitle', { name: del.name })} detail={tr('machines.deleteDetail')} okLabel={tr('machines.deleteOk')} cancelLabel={tr('machines.cancel')} destructive
        onOk={() => remove(del)} onCancel={() => setDel(null)} /> : null}
      {log ? (
        <View onClick={() => setLog(null)} style={{ position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.35)' }}>
          <View onClick={(e) => e.stopPropagation()} style={{ width: '86%', height: '80%', padding: spacing.l, gap: spacing.s, borderRadius: 12, backgroundColor: t.windowBg }}>
            <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('machines.logTitle', { name: log.name })}</Text>
            <ScrollView style={{ flex: 1 }} contentStyle={{ padding: spacing.xs }}>
              <Text style={{ fontFamily: 'monospace', fontSize: 11, color: t.text, textAlign: 'start', direction: 'ltr', whiteSpace: 'pre-wrap' }}>{log.text || tr('machines.logEmpty')}</Text>
            </ScrollView>
            <View style={{ flexDirection: 'row', justifyContent: 'flex-end' }}><Button label={tr('machines.close')} onClick={() => setLog(null)} /></View>
          </View>
        </View>
      ) : null}
    </View>
  );
}
