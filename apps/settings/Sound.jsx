// Settings → Sound: outputs, inputs (with meter), per-app volume (PipeWire, sdk/sound.js).
import {
  View, Text, useTheme, useLocale, Slider, Toggle, Button, Segmented, useSound, useSoundPeak, sound, defaultSink,
  microphones, defaultSource, spacing, typeScale,
} from '@lordos/sdk';
import { useEffect } from 'react';
import { Stack, Section, SettingRow, Note, tint } from './ui.jsx';

function Volume({ kind, item, width = 170 }) {
  const t = useTheme();
  const pct = Math.round(item.level * 100);
  // past 100 % the gain is digital: anything loud clips, whatever the card or headphones
  useEffect(() => { if (item.level > 1) sound.setVolume(kind, item.index, 1); }, [item.index, item.level > 1]);
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, direction: 'ltr' }}>
      <Slider value={Math.min(1, item.level)} max={1} width={width} onChange={(v) => sound.setVolume(kind, item.index, v)} />
      <Text style={{ width: 40, fontSize: typeScale.callout, color: t.textDim, textAlign: 'end' }}>{`${pct} %`}</Text>
      <Toggle small on={!item.mute} onChange={(on) => sound.setMute(kind, item.index, !on)} />
    </View>
  );
}

// Rows of devices (an array, so the section draws hairlines between them): the default one is
// checked; a click chooses it.
function deviceRows(kind, list, t) {
  return list.map((d) => (
    <SettingRow key={d.index} label={d.description || d.name} onClick={() => !d.default && sound.setDefault(kind, d.name)}
      labelStyle={{ whiteSpace: 'nowrap', overflow: 'hidden', fontWeight: d.default ? 600 : 400 }}>
      {d.state === 'running' ? <View style={{ width: 7, height: 7, borderRadius: 4, backgroundColor: tint.ok }} /> : null}
      <Text style={{ width: 16, fontSize: 14, fontWeight: 600, color: t.accent, textAlign: 'center' }}>{d.default ? '✓' : ''}</Text>
    </SettingRow>
  ));
}

function Ports({ kind, dev }) {
  if (!dev || dev.ports.length < 2) return null;
  return (
    <Segmented small value={dev.activePort} onChange={(p) => sound.setPort(kind, dev.index, p)}
      options={dev.ports.map((p) => [p.name, p.description || p.name])} />
  );
}

function Meter({ source }) {
  const t = useTheme();
  const peak = useSoundPeak();
  useEffect(() => { if (source) sound.meter(source.index); return () => sound.meter(-1); }, [source?.index]);
  const v = peak.source === source?.index ? Math.min(1, peak.level) : 0;
  const n = 16;
  // segmented level meter
  return (
    <View style={{ flexDirection: 'row', gap: 3, direction: 'ltr' }}>
      {Array.from({ length: n }, (_, i) => (
        <View key={i} style={{ width: 8, height: 8, borderRadius: 4,
          backgroundColor: i < Math.round(v * n) ? (i >= n - 3 ? tint.error : tint.ok) : t.control }} />
      ))}
    </View>
  );
}

export function SoundSettings() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const st = useSound();
  if (!sound.available() || !st.connected) {
    return <Stack><Note tone="warn">{tr('settings.sound.unavailable')}</Note></Stack>;
  }
  const out = defaultSink(st);
  const mics = microphones(st);
  const mic = defaultSource(st);
  const apps = st.inputs;
  return (
    <Stack>
      <Section title={tr('settings.sound.output')}>
        {st.sinks.length === 0 ? <SettingRow label={tr('settings.sound.noOutputs')} disabled /> : deviceRows('sink', st.sinks, t)}
      </Section>
      {out && (
        <Section>
          <SettingRow label={tr('settings.sound.volume')}><Volume kind="sink" item={out} /></SettingRow>
          {out.ports.length > 1 && <SettingRow label={tr('settings.sound.port')}><Ports kind="sink" dev={out} /></SettingRow>}
          <SettingRow label={tr('settings.sound.test')}><Button small label={tr('settings.sound.play')} onClick={() => sound.play('notify')} /></SettingRow>
        </Section>
      )}
      <Section title={tr('settings.sound.input')}>
        {mics.length === 0 ? <SettingRow label={tr('settings.sound.noInputs')} disabled /> : deviceRows('source', mics, t)}
      </Section>
      {mic && (
        <Section>
          <SettingRow label={tr('settings.sound.volume')}><Volume kind="source" item={mic} /></SettingRow>
          <SettingRow label={tr('settings.sound.level')}><Meter source={mic} /></SettingRow>
        </Section>
      )}
      <Section title={tr('settings.sound.apps')}>
        {apps.length === 0 ? <SettingRow label={tr('settings.sound.noApps')} disabled /> : apps.map((a) => (
          <SettingRow key={a.index} label={a.app || a.name}>
            {a.hasVolume ? <Volume kind="input" item={a} width={140} /> : <Toggle small on={!a.mute} onChange={(on) => sound.setMute('input', a.index, !on)} />}
          </SettingRow>
        ))}
      </Section>
    </Stack>
  );
}
