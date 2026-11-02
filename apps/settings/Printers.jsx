// Settings → Printers & Scanners. CUPS manages the queue and the drivers; this screen
// speaks IPP to it over its local socket with axia-server's own client (lord.print). Adding,
// removing and choosing the default are authorized by the CUPS policy (lpadmin group, which
// administrators have). Driverless printing (IPP Everywhere) printers and eSCL scanners on the network
// are found through mDNS. See docs/PRINTING.md.
import { View, Text, Button, TextInput, useTheme, wm, print, queueName, t as tr, spacing, typeScale } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { Stack, Section, SettingRow, FieldRow, Note, Actions, PageHeader, tint } from './ui.jsx';

function errText(e) {
  if (e.code === 'forbidden') return tr('print.err.forbidden');
  if (e.code === 'no-cups') return tr('print.noCups');
  if (e.code === 'not-found') return tr('print.err.notFound');
  return tr('print.err.other', { error: e.detail || e.code || e.message });
}

function stateText(p) {
  if (p.state === 5) return tr('print.state.stopped');
  if (p.state === 4) return tr('print.state.printing');
  if (!p.accepting) return tr('print.state.rejecting');
  return tr('print.state.idle');
}

const JOB_STATE = { 3: 'pending', 4: 'held', 5: 'processing', 6: 'stopped', 7: 'canceled', 8: 'aborted', 9: 'completed' };

// A flatbed scanner: a lid over a base, drawn (⌸ renders as a box).
function ScannerGlyph({ size = 26 }) {
  return (
    <View style={{ width: size, height: size, justifyContent: 'center' }}>
      <View style={{ height: size * 0.18, marginHorizontal: size * 0.04, borderRadius: 2, backgroundColor: '#8e8e93' }} />
      <View style={{ height: size * 0.32, marginTop: 1, borderRadius: size * 0.08, backgroundColor: '#636366' }} />
    </View>
  );
}

function PrinterGlyph({ size = 34, color = '#8e8e93' }) {
  return (
    <View style={{ width: size, height: size, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ position: 'absolute', top: size * 0.08, width: size * 0.5, height: size * 0.3, backgroundColor: '#ffffff', border: '1px solid rgba(0,0,0,0.25)' }} />
      <View style={{ position: 'absolute', top: size * 0.32, width: size * 0.86, height: size * 0.38, borderRadius: size * 0.08, backgroundColor: color }} />
      <View style={{ position: 'absolute', top: size * 0.6, width: size * 0.56, height: size * 0.3, backgroundColor: '#ffffff', border: '1px solid rgba(0,0,0,0.25)' }} />
    </View>
  );
}

export function PrintersSettings() {
  const th = useTheme();
  const [data, setData] = useState(null);           // { printers, default }
  const [jobs, setJobs] = useState([]);
  const [sel, setSel] = useState(null);
  const [error, setError] = useState(null);
  const [found, setFound] = useState(null);         // { printers, scanners }
  const [searching, setSearching] = useState(false);
  const [busy, setBusy] = useState(null);
  const [manual, setManual] = useState(null);       // { name, uri, info, location }
  const [msg, setMsg] = useState(null);
  const cups = print.hasCups();

  const reload = () => print.printers().then((r) => { setData(r); setError(null); if (!sel && r.printers[0]) setSel(r.default || r.printers[0].name); }).catch((e) => { setData({ printers: [], default: '' }); setError(errText(e)); });
  const reloadJobs = () => print.jobs().then((r) => setJobs(r.jobs)).catch(() => {});
  const discover = () => { setSearching(true); print.discover().then(setFound).catch(() => setFound({ printers: [], scanners: [] })).finally(() => setSearching(false)); };
  useEffect(() => { if (cups) { reload(); reloadJobs(); } discover(); }, []);
  useEffect(() => { if (!cups) return undefined; const id = setInterval(() => { reloadJobs(); reload(); }, 3000); return () => clearInterval(id); }, [sel]);
  useEffect(() => { if (!msg) return undefined; const id = setTimeout(() => setMsg(null), 5000); return () => clearTimeout(id); }, [msg]);

  const act = (key, p, ok) => { setBusy(key); p.then(() => { setMsg(ok); reload(); }).catch((e) => setMsg(errText(e))).finally(() => setBusy(null)); };
  const addFound = (f) => {
    const name = queueName(f.name);
    act(`add:${f.uri}`, print.add(name, f.uri, f.name, f.note || ''), tr('print.added', { name: f.name }));
  };
  const known = (uri) => data?.printers.some((p) => p.uri === uri);
  const printers = data?.printers || [];
  const cur = printers.find((p) => p.name === sel);
  const curJobs = jobs.filter((j) => !sel || j.printer === sel);

  if (!cups) {
    return (
      <Stack>
        <PageHeader icon={<PrinterGlyph size={44} />} title={tr('settings.section.printers')} subtitle={tr('print.noCupsLong')} />
      </Stack>
    );
  }
  const dimRow = (text) => <SettingRow label={text} labelStyle={{ color: th.textDim }} />;
  return (
    <Stack>
      {error ? <Note tone="error">{error}</Note> : null}
      {/* CUPS printers */}
      <Section>
        {data == null ? dimRow(tr('print.loading')) : !printers.length ? dimRow(tr('print.noPrinters')) : printers.map((p) => (
          <SettingRow key={p.name} onClick={() => setSel(p.name)} minHeight={48}
            icon={<PrinterGlyph size={30} color={p.state === 5 ? tint.error : p.state === 4 ? tint.ok : tint.off} />}
            label={p.info || p.name} labelStyle={{ fontWeight: sel === p.name ? 600 : 400 }}
            hint={[stateText(p), p.model, p.location, p.message].filter(Boolean).join(' · ')}>
            {data.default === p.name ? <View style={{ paddingHorizontal: 7, height: 18, borderRadius: 9, justifyContent: 'center', backgroundColor: th.accent }}>
              <Text style={{ fontSize: 10, fontWeight: 600, color: '#fff' }}>{tr('print.defaultBadge')}</Text></View> : null}
            {p.queued ? <Text style={{ fontSize: typeScale.caption, color: th.textDim }}>{tr('print.queued', { count: p.queued })}</Text> : null}
            <Text style={{ width: 16, fontSize: 14, fontWeight: 600, color: th.accent, textAlign: 'center' }}>{sel === p.name ? '✓' : ''}</Text>
          </SettingRow>
        ))}
      </Section>
      {cur && (
        <Actions>
          <Button small label={tr('print.setDefault')} onClick={() => act('default', print.setDefault(cur.name), tr('print.defaultSet', { name: cur.info || cur.name }))} />
          <Button small label={tr('print.testPage')} onClick={() => print.open({ title: tr('print.testTitle'), text: tr('print.testText', { name: cur.info || cur.name, model: cur.model || '' }) })} />
          <Button small destructive label={busy === 'remove' ? tr('print.removing') : tr('print.remove')} onClick={() => act('remove', print.remove(cur.name), tr('print.removed', { name: cur.info || cur.name }))} />
        </Actions>
      )}
      {/* queue */}
      {cur && (
        <Section title={tr('print.queueOf', { name: cur.info || cur.name })}>
          {!curJobs.length ? dimRow(tr('print.queueEmpty')) : curJobs.map((j) => (
            <SettingRow key={j.id} icon={<Text style={{ width: 36, fontSize: typeScale.caption, color: th.textDim, textAlign: 'start' }}>{`#${j.id}`}</Text>}
              label={j.name || tr('print.untitled')} labelStyle={{ whiteSpace: 'nowrap', overflow: 'hidden' }}
              hint={`${j.user} · ${tr(`print.job.${JOB_STATE[j.state] || 'pending'}`)}${j.kb ? ` · ${j.kb} KB` : ''}`}>
              <Button small label={tr('print.cancelJob')} onClick={() => act(`c${j.id}`, print.cancel(j.id).then(reloadJobs), tr('print.jobCanceled', { id: j.id }))} />
            </SettingRow>
          ))}
        </Section>
      )}
      {/* add */}
      <Section title={tr('print.addTitle')} footer={tr('print.addHint')} accessory={(
        <View style={{ flexDirection: 'row', gap: spacing.s }}>
          <Button small label={searching ? tr('print.searching') : tr('print.searchAgain')} onClick={discover} />
          <Button small label={tr('print.manual')} onClick={() => setManual(manual ? null : { name: '', uri: 'ipp://', info: '', location: '' })} />
        </View>
      )}>
        {found?.printers.length ? found.printers.map((f) => (
          <SettingRow key={f.uri} icon={<PrinterGlyph size={26} />} label={f.name}
            hint={[f.model, f.uri, f.color === 'T' ? tr('print.color') : null, f.duplex === 'T' ? tr('print.duplex') : null].filter(Boolean).join(' · ')}>
            {known(f.uri) ? <Text style={{ fontSize: typeScale.caption, color: th.textDim }}>{tr('print.alreadyAdded')}</Text>
              : <Button small label={busy === `add:${f.uri}` ? tr('print.adding') : tr('print.add')} onClick={() => addFound(f)} />}
          </SettingRow>
        )) : dimRow(found ? tr('print.noneFound') : tr('print.searching'))}
      </Section>
      {manual && (
        <View style={{ gap: spacing.m }}>
          <Section>
            <FieldRow label="URI" width={280}>
              <TextInput value={manual.uri} onChange={(v) => setManual({ ...manual, uri: v })} placeholder="ipp://192.168.1.20/ipp/print" style={{ flex: 1 }} />
            </FieldRow>
            <FieldRow label={tr('print.fieldName')} width={280}>
              <TextInput value={manual.info} onChange={(v) => setManual({ ...manual, info: v })} placeholder={tr('print.fieldName')} style={{ flex: 1 }} />
            </FieldRow>
            <FieldRow label={tr('print.fieldLocation')} width={280}>
              <TextInput value={manual.location} onChange={(v) => setManual({ ...manual, location: v })} placeholder={tr('print.fieldLocation')} style={{ flex: 1 }} />
            </FieldRow>
          </Section>
          <Actions>
            <Button label={tr('print.cancel')} onClick={() => setManual(null)} />
            <Button primary label={busy === 'manual' ? tr('print.adding') : tr('print.add')} onClick={() => {
              const info = manual.info.trim() || manual.uri.replace(/^\w+:\/\//, '');
              act('manual', print.add(queueName(info), manual.uri.trim(), info, manual.location.trim()), tr('print.added', { name: info }));
              setManual(null);
            }} />
          </Actions>
        </View>
      )}
      {/* scanners */}
      <Section title={tr('print.scanners')}>
        {found?.scanners.length ? found.scanners.map((s) => (
          <SettingRow key={`${s.addr}:${s.port}`} icon={<ScannerGlyph />} label={s.name}
            hint={[s.model, `${s.addr}:${s.port}`, s.tls ? tr('print.tlsUnsupported') : null].filter(Boolean).join(' · ')}>
            {!s.tls && s.lan ? <Button small label={tr('print.scan')} onClick={() => wm.open('scanner', { args: { scanner: s } })} /> : null}
          </SettingRow>
        )) : dimRow(found ? tr('print.noScanners') : tr('print.searching'))}
      </Section>
      {msg ? <Note style={{ color: th.accent }}>{msg}</Note> : null}
    </Stack>
  );
}
