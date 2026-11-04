// LordOS Imager: writes LordOS to an SD card or a USB stick and prepares the Raspberry Pi's first
// boot (computer name, user, Wi-Fi, remote access, look, cooling). Steps: Device → System image →
// Storage → Customise → Write → Done, with Lumo giving a tip at each one. docs/IMAGER.md.
//
// The same files build the desktop app (desktop/imager, Tauri): only platform.js is swapped and the
// few SDK primitives used are DOM versions there. Nothing here talks to the system directly.
import { View, Text, ScrollView, Button, Lumo, useTheme, useLocale, useStore, typeScale, spacing } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { platform } from './platform.js';
import { app, set, setOptions, STEPS, loadReleases, isBusy } from './state.js';
import { deviceById, deviceLabel, LANGUAGES } from './core/catalog.js';
import { Press, Helper, useFocusKeys } from './ui/kit.jsx';
import { ImagerIcon } from './ui/art.jsx';
import { DeviceStep } from './ui/StepDevice.jsx';
import { ImageStep } from './ui/StepImage.jsx';
import { StorageStep } from './ui/StepStorage.jsx';
import { CustomiseStep, customiseProblems } from './ui/StepCustomise.jsx';
import { WriteStep, WriteDialogs } from './ui/StepWrite.jsx';
import { DoneStep } from './ui/StepDone.jsx';
import { OptionsDialog } from './ui/Options.jsx';
import { formatSize } from './ui/format.js';

export const Icon = ImagerIcon;

// Lumo's pose and tip for each step
const HELP = {
  device: 'wave', image: 'shield', custom: 'confused', storage: 'wave', erase: 'shield', customise: 'wave', write: 'shield',
  writing: 'run', failed: 'confused', cancelled: 'shield', untouched: 'confused', 'tab.general': 'wave', 'tab.user': 'shield', 'tab.wifi': 'wave', 'tab.remote': 'shield',
  'tab.look': 'celebrate', 'tab.cooling': 'run', 'tab.more': 'shield',
};

function canNext(s) {
  if (s.step === 'device') return !!deviceById(s.device)?.supported;
  if (s.step === 'image') return !!s.image;
  if (s.step === 'storage') return !!s.drive;
  if (s.step === 'customise') return !s.customise || customiseProblems(s).length === 0;
  return false;
}

function next() {
  const s = app.get();
  if (s.step === 'customise' && s.customise && customiseProblems(s).length) { set({ showErrors: true }); return; }
  if (!canNext(s)) return;
  const dev = deviceById(s.device);
  let i = STEPS.indexOf(s.step) + 1;
  if (STEPS[i] === 'customise' && !dev.customise) i++;
  set({ step: STEPS[i], showErrors: false });
}
function back() {
  const s = app.get();
  if (isBusy(s.job)) return;
  const dev = deviceById(s.device);
  let i = STEPS.indexOf(s.step) - 1;
  if (STEPS[i] === 'customise' && !dev.customise) i--;
  if (i >= 0) set({ step: STEPS[i], job: s.step === 'write' && s.job?.phase === 'failed' ? null : s.job });
}

function stepDetail(s, id, tr) {
  if (id === 'device') return deviceLabel(deviceById(s.device), tr);
  if (id === 'image') return s.image ? (s.image.official ? `LordOS ${s.image.release.version}` : s.image.name) : '';
  if (id === 'storage') return s.drive ? `${s.drive.model} · ${formatSize(s.drive.size)}` : '';
  if (id === 'customise') return s.customise ? (s.cfg.hostname || tr('imager.custom.on')) : tr('imager.custom.skipped');
  return '';
}

// The desktop app's own language (on LordOS the Imager follows Settings): a globe with the
// language's name; its menu lists English, Español and Português, then the others, then "the
// computer's language".
function AppLanguage({ s }) {
  const t = useTheme();
  const { t: tr, lang } = useLocale();
  const [open, setOpen] = useState(false);
  const pick = (id) => { setOpen(false); setOptions({ lang: id }); platform.setLanguage?.(id); };
  const cur = s.options.lang || '';
  const name = LANGUAGES.find((l) => l.id === lang)?.name || lang;
  const item = (id, label) => (
    <Press key={id || 'auto'} id={`applang-${id || 'auto'}`} label={label} role="menuitemradio" selected={cur === id} onClick={() => pick(id)}
      style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 8, height: 28, borderRadius: 6,
        backgroundColor: cur === id ? t.accent : 'transparent' }}>
      <Text style={{ fontSize: typeScale.callout, color: cur === id ? '#fff' : t.text }}>{label}</Text>
    </Press>
  );
  const sep = (k) => <View key={k} style={{ height: 1, marginVertical: 4, marginHorizontal: 6, backgroundColor: t.separator }} />;
  return (
    <View>
      {open ? (
        <View {...platform.a11y({ role: 'menu', label: tr('imager.options.language') })}
          style={{ position: 'absolute', left: 0, right: 0, bottom: 36, padding: 6, borderRadius: 10, backgroundColor: t.windowBg,
            border: `1px solid ${t.separator}`, boxShadow: '0 12px 30px rgba(0,0,0,0.35)' }}>
          <Text style={{ fontSize: 11, fontWeight: 600, color: t.textDim, paddingHorizontal: 8, paddingVertical: 4 }}>{tr('imager.options.language')}</Text>
          {LANGUAGES.filter((l) => l.core).map((l) => item(l.id, l.name))}
          {sep('a')}
          {LANGUAGES.filter((l) => !l.core).map((l) => item(l.id, l.name))}
          {sep('b')}
          {item('', tr('imager.options.auto'))}
        </View>
      ) : null}
      <Press id="applang" label={`${tr('imager.options.language')}: ${name}`} onClick={() => setOpen(!open)}
        style={{ flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 12, paddingHorizontal: 8, paddingVertical: 6, borderRadius: 7 }}>
        <Globe color={t.textDim} />
        <Text style={{ fontSize: typeScale.callout, color: t.textDim }}>{name}</Text>
      </Press>
    </View>
  );
}

function Globe({ color }) {
  return (
    <View style={{ width: 14, height: 14, borderRadius: 7, border: `1.3px solid ${color}`, alignItems: 'center', justifyContent: 'center', overflow: 'hidden' }}>
      <View style={{ position: 'absolute', width: 6, height: 14, borderRadius: 3, border: `1.1px solid ${color}` }} />
      <View style={{ position: 'absolute', width: 14, height: 1.1, backgroundColor: color }} />
    </View>
  );
}

function Sidebar({ s }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const dev = deviceById(s.device);
  const steps = STEPS.filter((id) => id !== 'customise' || dev.customise);
  const cur = s.step === 'done' ? steps.length : steps.indexOf(s.step);
  const writing = isBusy(s.job) || s.job?.phase === 'failed';
  let tip = s.step === 'write' && writing ? (s.job.phase === 'failed' ? (s.job.cancelledByUser ? 'cancelled' : s.job.wrote ? 'failed' : 'untouched') : 'writing') : s.step;
  if (s.step === 'storage' && s.drive) tip = 'erase';
  if (s.step === 'image' && s.image && !s.image.official) tip = 'custom';
  if (s.step === 'customise' && s.customise) tip = `tab.${s.tab}`;
  const pose = HELP[tip] || HELP[s.step] || 'wave';
  return (
    <View style={{ width: 236, backgroundColor: t.sidebarBg, borderEndWidth: 1, borderEndColor: t.separator, paddingHorizontal: 12, paddingTop: 18, paddingBottom: 14 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 6, marginBottom: 20 }}>
        <ImagerIcon size={34} />
        <View style={{ flex: 1 }}>
          <Text style={{ fontSize: 14, fontWeight: 700, color: t.text }}>{tr('imager.title')}</Text>
          <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr('imager.subtitle')}</Text>
        </View>
      </View>
      <View style={{ gap: 2 }} {...platform.a11y({ role: 'list', label: tr('imager.steps') })}>
        {steps.map((id, i) => {
          const done = i < cur, here = i === cur;
          const reachable = !writing && s.step !== 'done' && i <= cur;
          return (
            <Press key={id} id={`step-${id}`} label={tr(`imager.step.${id}`)} role="tab" selected={here} disabled={!reachable && !here}
              onClick={reachable ? () => set({ step: id }) : undefined}
              style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 8, paddingVertical: 7, borderRadius: 8,
                backgroundColor: here ? (t.mode === 'dark' ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.05)') : 'transparent', opacity: 1 }}>
              <View style={{ width: 22, height: 22, borderRadius: 11, alignItems: 'center', justifyContent: 'center',
                backgroundColor: done ? t.accent : here ? 'transparent' : t.control, border: here ? `2px solid ${t.accent}` : 'none' }}>
                <Text style={{ fontSize: 11, fontWeight: 700, color: done ? '#fff' : here ? t.accent : t.textDim }}>{done ? '✓' : String(i + 1)}</Text>
              </View>
              <View style={{ flex: 1, minWidth: 0 }}>
                <Text style={{ fontSize: typeScale.body, fontWeight: here ? 600 : 500, color: here || done ? t.text : t.textDim }}>{tr(`imager.step.${id}`)}</Text>
                {stepDetail(s, id, tr) && (done || here) ? (
                  <Text style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{stepDetail(s, id, tr)}</Text>
                ) : null}
              </View>
            </Press>
          );
        })}
      </View>
      <View style={{ flex: 1 }} />
      {s.step !== 'done' ? <Helper pose={pose} title={tr(`imager.tip.${tip}.title`)} text={tr(`imager.tip.${tip}.text`)} /> : null}
      {!platform.followsSystem ? <AppLanguage s={s} /> : null}
      <Press id="options" label={tr('imager.options.title')} onClick={() => set({ dialog: 'options' })}
        style={{ flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 12, paddingHorizontal: 8, paddingVertical: 6, borderRadius: 7 }}>
        <Text style={{ fontSize: 14, color: t.textDim }}>⚙</Text>
        <Text style={{ fontSize: typeScale.callout, color: t.textDim }}>{tr('imager.options.title')}</Text>
      </Press>
    </View>
  );
}

function Footer({ s }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  if (s.step === 'done' || (s.step === 'write' && s.job)) return null;
  const first = s.step === 'device';
  const isWrite = s.step === 'write';
  return (
    <View style={{ height: 56, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingHorizontal: 24, borderTopWidth: 1, borderTopColor: t.separator }}>
      <View style={{ flex: 1 }} />
      {!first ? <Button label={tr('imager.back')} onClick={back} /> : null}
      {isWrite ? <Button primary label={tr('imager.write.start')} style={{ backgroundColor: '#e5484d' }} onClick={() => set({ dialog: 'confirm' })} />
        : <Button primary label={tr('imager.next')} onClick={next} disabled={!canNext(s) && !(s.step === 'customise')} />}
    </View>
  );
}

export default function Imager() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const s = useStore(app);
  useFocusKeys({ onEscape: back });
  useEffect(() => { loadReleases(); }, []);
  if (!platform.available()) {
    return (
      <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 10, padding: 30, backgroundColor: t.windowBg }}>
        <Lumo pose="confused" size={112} />
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text }}>{tr('imager.unavailable.title')}</Text>
        <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'center', maxWidth: 380 }}>{tr('imager.unavailable.text')}</Text>
      </View>
    );
  }
  const Step = { device: DeviceStep, image: ImageStep, storage: StorageStep, customise: CustomiseStep, write: WriteStep, done: DoneStep }[s.step];
  const Layer = platform.Layer;
  return (
    <View style={{ flex: 1, flexDirection: 'row', backgroundColor: t.windowBg }}>
      <Sidebar s={s} />
      <View style={{ flex: 1, minWidth: 0 }}>
        {s.step === 'customise' || s.step === 'done' ? <Step s={s} /> : (
          <ScrollView style={{ flex: 1 }} contentStyle={{ paddingHorizontal: 28, paddingTop: 24, paddingBottom: 24 }}>
            <Step s={s} />
          </ScrollView>
        )}
        <Footer s={s} />
      </View>
      {s.dialog === 'options' ? <OptionsDialog s={s} /> : null}
      <WriteDialogs s={s} />
      {Layer ? <Layer /> : null}
    </View>
  );
}
