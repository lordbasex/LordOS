// Settings → AI → Sentinela (docs/SENTINELA.md): the process guard. Off by default. When on: the local model it uses
// (installed lordos-llm-* packages, or a button that downloads one with the engine, sdk/llmcatalog.js), the triggers
// (the ready ones; the others "coming soon"), its limits, what it reads, the alerts with their facts and the closed
// list of actions, and the programs marked "It's mine". The virtual machines' alerts (sdk/sentinelavm.js) have their own
// actions: open the machine, show it in Monitor, pause or resume, shut down.
import {
  View, Text, Button, Toggle, useTheme, useLocale, useStore, formatBytes, formatTime, formatDate, spacing, typeScale,
  LLM_MODELS, packagesForModel, TRIGGERS, ACTIONS, VM_ACTIONS, vmWhy, vmFactLines, factLines, presentAlert, sentinelaAlerts, sentinelaEngine, sentinelaPrefs, setSentinelaPrefs,
  refreshSentinelaEngine, runSentinelaAction, sentinelaOnBattery, clearSentinelaAlerts, sentinelaActionLabel, installLlm, removeLlm,
  ShieldGlyph, settings, fs,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { Section, SettingRow, Note, tint } from './ui.jsx';

const VERDICT_COLOR = { dangerous: tint.error, suspicious: tint.warn, normal: tint.ok };

export function SentinelaSettings({ alertId }) {
  const { t: tr } = useLocale();
  const th = useTheme();
  useStore(settings);
  const p = sentinelaPrefs();
  const eng = useStore(sentinelaEngine);
  const alerts = useStore(sentinelaAlerts);
  const [job, setJob] = useState(null);            // { id, done, total } while a model downloads
  const [msg, setMsg] = useState(null);
  const [open, setOpen] = useState(alertId || null);
  useEffect(() => { refreshSentinelaEngine(); }, [p.on]);
  useEffect(() => { if (alertId) setOpen(alertId); }, [alertId]);
  useEffect(() => {
    if (!p.on) return undefined;
    const id = setInterval(refreshSentinelaEngine, 3000);       // the engine's state while the page is open
    return () => clearInterval(id);
  }, [p.on]);

  const installed = new Set((eng.models || []).map((m) => m.id));
  const chosen = p.model && installed.has(p.model) ? p.model : null;
  const download = async (m) => {
    if (job) return;
    setMsg(null);
    setJob({ id: m.id, done: 0, total: 0 });
    try {
      await installLlm(packagesForModel(m.id, eng.available), (pr) => setJob({ id: m.id, ...pr }));
      const v = refreshSentinelaEngine();
      if (!chosen && v.models.some((x) => x.id === m.id)) setSentinelaPrefs({ model: m.id });
    } catch (e) { setMsg({ ok: false, text: e.message }); }
    setJob(null);
  };
  const remove = async (m) => {
    if (job) return;
    setMsg(null);
    try { await removeLlm([m.package]); } catch (e) { setMsg({ ok: false, text: e.message }); }
    if (p.model === m.id) setSentinelaPrefs({ model: null });
    refreshSentinelaEngine();
  };
  const pct = job && job.total ? Math.floor((job.done * 100) / job.total) : 0;

  const modelRow = (m) => {
    const has = installed.has(m.id), inUse = chosen === m.id;
    const hint = [m.quant, formatBytes(m.bytes), tr('sentinela.settings.ram', { size: formatBytes(m.ramMb * 1048576) }),
      m.recommended ? tr('sentinela.settings.recommended') : null, inUse ? tr('sentinela.settings.inUse') : null].filter(Boolean).join(' · ');
    return (
      <SettingRow key={m.id} label={m.name} hint={hint}>
        {job && job.id === m.id ? <Text style={{ fontSize: typeScale.caption, color: th.textDim }}>{tr('sentinela.settings.downloading', { pct })}</Text>
          : has ? (
            <>
              {!inUse ? <Button small label={tr('sentinela.settings.use')} onClick={() => setSentinelaPrefs({ model: m.id })} /> : null}
              <Button small destructive label={tr('sentinela.settings.remove')} disabled={!!job} onClick={() => remove(m)} />
            </>
          ) : <Button small label={tr('sentinela.settings.download', { size: formatBytes(m.bytes) })} disabled={!!job} onClick={() => download(m)} />}
      </SettingRow>
    );
  };

  const engineState = eng.state && eng.state.state ? tr(`sentinela.engine.${eng.state.state}`) : tr('sentinela.engine.off');
  return (
    <>
      <Section title={tr('sentinela.name')} footer={tr('sentinela.settings.footer')}>
        <SettingRow icon={<ShieldGlyph size={28} />} label={tr('sentinela.settings.on')} hint={tr('sentinela.settings.onHint')}>
          <Toggle small on={p.on} onChange={(on) => setSentinelaPrefs({ on })} />
        </SettingRow>
      </Section>

      {p.on ? (
        <>
          <Section title={tr('sentinela.settings.model')} footer={tr('sentinela.settings.modelFooter')}>
            {LLM_MODELS.map(modelRow)}
            {!chosen ? <Note>{tr(installed.size ? 'sentinela.settings.pickModel' : 'sentinela.settings.noModel')}</Note> : null}
            {chosen ? (
              <SettingRow label={tr('sentinela.settings.noAnalysis')} hint={tr('sentinela.settings.noAnalysisHint')}>
                <Button small label={tr('sentinela.settings.noAnalysisButton')} onClick={() => setSentinelaPrefs({ model: null })} />
              </SettingRow>
            ) : null}
          </Section>

          <Section title={tr('sentinela.settings.triggers')} footer={tr('sentinela.settings.triggersFooter')}>
            {TRIGGERS.map((x) => (
              <SettingRow key={x.id} label={tr(`sentinela.trigger.${x.id}`)} hint={x.always ? tr('sentinela.settings.always') : x.ready ? null : tr('sentinela.settings.soon')}
                disabled={!x.ready || x.always}>
                <Toggle small on={x.always || (x.ready && p.triggers[x.id] !== false)}
                  onChange={(on) => x.ready && !x.always && setSentinelaPrefs({ triggers: { ...p.triggers, [x.id]: on } })} />
              </SettingRow>
            ))}
          </Section>

          <Section title={tr('sentinela.settings.limits')}>
            <SettingRow label={tr('sentinela.settings.when')}><Text style={{ fontSize: typeScale.callout, color: th.textDim }}>{tr('sentinela.settings.whenValue')}</Text></SettingRow>
            <SettingRow label={tr('sentinela.settings.max')}><Text style={{ fontSize: typeScale.callout, color: th.textDim }}>{tr('sentinela.settings.maxValue')}</Text></SettingRow>
            <SettingRow label={tr('sentinela.settings.battery')} hint={sentinelaOnBattery() ? tr('sentinela.settings.onBatteryNow') : null}>
              <Text style={{ fontSize: typeScale.callout, color: th.textDim }}>{tr('sentinela.settings.batteryValue')}</Text></SettingRow>
            <SettingRow label={tr('sentinela.settings.engine')} hint={eng.available ? null : tr('sentinela.settings.engineMissing')}>
              <Text style={{ fontSize: typeScale.callout, color: th.textDim }}>{engineState}</Text></SettingRow>
          </Section>

          <Section title={tr('sentinela.settings.sees')}>
            <Text style={{ fontSize: typeScale.callout, color: th.text, lineHeight: 1.45, textAlign: 'start', paddingVertical: spacing.s }}>{tr('sentinela.settings.seesText')}</Text>
            <Text style={{ fontSize: typeScale.callout, color: th.text, lineHeight: 1.45, textAlign: 'start', paddingVertical: spacing.s }}>{tr('sentinela.settings.seesMachines')}</Text>
          </Section>

          <Section title={tr('sentinela.settings.history')} accessory={alerts.length ? <Button small label={tr('sentinela.settings.clear')} onClick={clearSentinelaAlerts} /> : null}>
            {alerts.length ? alerts.map((a) => <AlertRow key={a.id} a={a} open={open === a.id} onToggle={() => setOpen(open === a.id ? null : a.id)} />)
              : <SettingRow label={tr('sentinela.settings.noAlerts')} />}
          </Section>

          <Section title={tr('sentinela.settings.ignored')} footer={tr('sentinela.settings.ignoredFooter')}>
            {p.ignore.length ? p.ignore.map((x) => (
              <SettingRow key={`${x.exe}|${x.pkg}`} label={x.name || x.exe} hint={[x.exe, x.pkg].filter(Boolean).join(' · ')}>
                <Button small label={tr('sentinela.settings.unignore')} onClick={() => setSentinelaPrefs({ ignore: p.ignore.filter((y) => y !== x) })} />
              </SettingRow>
            )) : <SettingRow label={tr('sentinela.settings.noIgnored')} />}
          </Section>
        </>
      ) : null}
      {msg ? <Note tone={msg.ok ? 'ok' : 'error'}>{msg.text}</Note> : null}
    </>
  );
}

function AlertRow({ a, open, onToggle }) {
  const { t: tr } = useLocale();
  const th = useTheme();
  const when = new Date(a.time || 0);
  const pres = presentAlert(a);
  const title = tr(pres.title, { name: a.name });
  const home = (() => { try { return fs.home(); } catch (e) { return ''; } })();
  const vm = a.kind === 'machine';
  const lines = vm ? vmFactLines(a.ev || a, tr) : factLines(a.ev || a, a.facts, { home, user: home.split('/').filter(Boolean).pop() || '' }).map((l) => tr(l.key, l.vars));
  const acts = vm ? [...VM_ACTIONS.filter((x) => x !== 'vm_pause' || a.done !== 'vm_pause'), ...(a.done === 'vm_pause' ? ['vm_resume'] : [])]
    : a.rule === 'autostart' ? ['monitor', 'ignore'] : [...ACTIONS.filter((x) => x !== 'pause' || a.done !== 'pause'), ...(a.done === 'pause' ? ['resume'] : [])];
  const why = pres.reason || (vm ? vmWhy(a.ev || a, tr) : tr(`sentinela.why.${a.rule}`, { name: a.name, place: tr(`sentinela.place.${a.place || 'none'}`) }));
  return (
    <View style={{ paddingVertical: spacing.s, gap: spacing.xs }}>
      <View onClick={onToggle} style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
        <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: VERDICT_COLOR[a.verdict] || tint.off }} />
        <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.body, color: th.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{title}</Text>
        <Text style={{ fontSize: typeScale.caption, color: th.textDim }}>{`${formatDate(when)} ${formatTime(when)}`}</Text>
      </View>
      {open ? (
        <View style={{ gap: spacing.xs, paddingStart: 16 }}>
          <Text style={{ fontSize: typeScale.callout, color: th.text, lineHeight: 1.45, textAlign: 'start' }}>
            {why}</Text>
          {pres.note ? <Text style={{ fontSize: typeScale.caption, color: th.textDim, textAlign: 'start' }}>{tr(pres.note)}</Text> : null}
          <Text style={{ fontSize: typeScale.caption, color: th.textDim, textAlign: 'start' }}>{tr('sentinela.analyzed')}</Text>
          {lines.map((l, i) => <Text key={i} style={{ fontSize: typeScale.caption, color: th.text, textAlign: 'start' }}>{l}</Text>)}
          <Text style={{ fontSize: typeScale.caption, color: th.textDim, textAlign: 'start' }}>{tr(`sentinela.analysis.${a.analysis || 'none'}`)}</Text>
          {a.done ? <Text style={{ fontSize: typeScale.caption, color: tint.ok, textAlign: 'start' }}>{tr('sentinela.done', { action: sentinelaActionLabel(a.done) })}</Text> : null}
          {a.error ? <Text style={{ fontSize: typeScale.caption, color: tint.error, textAlign: 'start' }}>{tr(`errors.${a.error}`)}</Text> : null}
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: spacing.s, marginTop: spacing.xs }}>
            {acts.map((id) => <Button key={id} small primary={id === a.suggested} destructive={id === 'end_block' || id === 'vm_shutdown'} label={sentinelaActionLabel(id)}
              onClick={() => runSentinelaAction(a, id)} />)}
          </View>
          <Text style={{ fontSize: typeScale.caption, color: th.textDim, textAlign: 'start' }}>{tr('sentinela.youDecide')}</Text>
        </View>
      ) : null}
    </View>
  );
}
