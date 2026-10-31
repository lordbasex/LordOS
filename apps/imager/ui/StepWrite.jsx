// Step 5 — review, confirm and write: download (resumable) → signature → write → read back and
// compare → first-run file → eject. Progress, speed and time left; cancelling is safe (the card is
// wiped so it never looks half-bootable).
import { View, Text, Button, useTheme, useLocale, typeScale, spacing } from '@lordos/sdk';
import { platform } from '../platform.js';
import { app, set, run, cancel, isBusy } from '../state.js';
import { deviceById, deviceLabel, COOLERS } from '../core/catalog.js';
import { Group, KeyValue, Notice, ProgressBar, Badge, useTick, Press } from './kit.jsx';
import { DriveArt } from './art.jsx';
import { StepHeader } from './StepDevice.jsx';
import { formatSize, formatSpeed, formatEta } from './format.js';

function summary(s, tr) {
  const c = s.cfg;
  const rows = [];
  if (c.hostname) rows.push([tr('imager.review.host'), `${c.hostname}.local`]);
  if (c.user.name) rows.push([tr('imager.review.user'), `${c.user.name}${c.user.fullname ? ` (${c.user.fullname})` : ''}${c.user.autologin ? ` · ${tr('imager.review.autologin')}` : ''}`]);
  rows.push([tr('imager.review.locale'), [c.lang, c.timezone, c.keyboard ? `${c.keyboard}${c.variant ? `(${c.variant})` : ''}` : '', c.wifiCountry].filter(Boolean).join(' · ')]);
  if (c.wifi.enabled) rows.push([tr('imager.review.wifi'), `${c.wifi.ssid}${c.wifi.open ? ` · ${tr('imager.wifi.open')}` : ''}${c.wifi.hidden ? ` · ${tr('imager.review.hidden')}` : ''}`]);
  if (c.user.name) rows.push([tr('imager.review.ssh'), c.ssh === 'off' ? tr('imager.remote.sshOff') : `${tr(c.ssh === 'keys' ? 'imager.remote.sshKeys' : 'imager.remote.sshPassword')}${c.sshKeys.length ? ` · ${tr('imager.review.keys', { n: c.sshKeys.length })}` : ''}`]);
  if (c.user.name) rows.push([tr('imager.review.remote'), tr(c.remote ? 'imager.review.remoteOn' : 'imager.review.off')]);
  rows.push([tr('imager.review.look'), `${tr(`imager.look.theme.${c.theme}`)} · ${tr(`imager.look.${c.mode}`)} · ${tr(`imager.look.accents.${c.accent}`)}`]);
  if (c.cooler) rows.push([tr('imager.review.cooling'), `${tr(`imager.cooling.${c.cooler}`)} · ${tr(`imager.cooling.p.${c.performance}`)}${c.overclock ? ` · ${tr('imager.review.overclock')}` : ''}`]);
  rows.push([tr('imager.review.more'), `${tr('imager.more.bluetooth')}: ${tr(c.bluetooth ? 'imager.review.on' : 'imager.review.off')} · ${tr('imager.more.updates')}: ${tr(c.autoUpdates ? 'imager.review.on' : 'imager.review.off')}`]);
  return rows;
}

function Review({ s }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const dev = deviceById(s.device);
  const img = s.image;
  const custom = s.customise && dev.customise;
  return (
    <View>
      <StepHeader title={tr('imager.review.title')} text={tr('imager.review.text')} />
      <Group>
        <KeyValue label={tr('imager.step.device')} value={deviceLabel(dev, tr)} />
        <KeyValue label={tr('imager.step.image')} value={img.official ? `LordOS ${img.release.version} · ${formatSize(img.size)}` : `${img.name} · ${formatSize(img.size)}`} />
        <KeyValue label={tr('imager.step.storage')} value={`${s.drive.model} · ${formatSize(s.drive.size)} (${s.drive.name})`} />
        <KeyValue label={tr('imager.review.check')} value={tr(img.official ? 'imager.review.checkOfficial' : 'imager.review.checkCustom')} />
      </Group>
      {custom ? (
        <Group title={tr('imager.review.firstBoot')} style={{ marginTop: spacing.xl }}>
          {summary(s, tr).map(([k, val]) => <KeyValue key={k} label={k} value={val} />)}
        </Group>
      ) : dev.customise ? <Notice tone="info" title={tr('imager.custom.skipTitle')} style={{ marginTop: spacing.xl }}>{tr('imager.custom.skipText')}</Notice> : null}
      {!img.official ? <Notice tone="warn" title={tr('imager.image.customWarnTitle')} style={{ marginTop: spacing.l }}>{tr('imager.image.customWarn')}</Notice> : null}
      <Notice tone="bad" title={tr('imager.storage.eraseTitle', { name: s.drive.model })} style={{ marginTop: spacing.l }}>{tr('imager.storage.eraseText', { size: formatSize(s.drive.size) })}</Notice>
    </View>
  );
}

const PHASES = ['signature', 'download', 'check', 'write', 'verify', 'firstrun', 'eject'];

function Progress({ s }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  useTick(1000, isBusy(s.job));
  const j = s.job;
  const dev = deviceById(s.device);
  const custom = s.customise && dev.customise;
  const shown = PHASES.filter((p) => (p !== 'signature' && p !== 'download') || s.image.official).filter((p) => p !== 'check' || s.image.official)
    .filter((p) => p !== 'firstrun' || custom).filter((p) => p !== 'eject' || s.options.eject);
  const cur = j.phase === 'secrets' || j.phase === 'prepare' ? 'write' : j.phase;
  const curIdx = shown.indexOf(cur);
  const frac = j.total ? j.done / j.total : 0;
  const failed = j.phase === 'failed';
  const msg = j.error?.message;
  return (
    <View>
      <StepHeader title={tr(failed ? (j.cancelledByUser ? 'imager.write.cancelledTitle' : 'imager.write.failedTitle') : 'imager.write.title', { name: s.drive.model })}
        text={tr(failed ? (!j.wrote ? 'imager.write.untouchedText' : j.cancelledByUser ? 'imager.write.cancelledText' : 'imager.write.failedText') : 'imager.write.text')} />
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.l, marginBottom: spacing.xl }}>
        <DriveArt transport={s.drive.transport} size={48} />
        <View style={{ flex: 1, gap: 8 }}>
          <View style={{ flexDirection: 'row', alignItems: 'baseline', gap: 8 }}>
            <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text }}>{tr(`imager.phase.${cur}`)}</Text>
            <View style={{ flex: 1 }} />
            {j.total && !failed ? <Text style={{ fontSize: typeScale.callout, color: t.textDim }}>{`${Math.floor(frac * 100)} %`}</Text> : null}
          </View>
          <ProgressBar value={failed ? 0 : frac} height={10} />
          {!failed && j.total ? (
            <View style={{ flexDirection: 'row', gap: spacing.l }}>
              <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr('imager.write.amount', { done: formatSize(j.done), total: formatSize(j.total) })}</Text>
              <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr('imager.write.speed', { speed: formatSpeed(j.speed) })}</Text>
              <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr('imager.write.eta', { eta: formatEta(j.eta) })}</Text>
            </View>
          ) : null}
        </View>
      </View>
      <Group>
        {shown.map((p, i) => {
          const done = (j.phases || []).includes(p) || (curIdx >= 0 && i < curIdx && !failed);
          const here = p === cur && !failed;
          return (
            <View key={p} style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m, minHeight: 34 }} {...platform.a11y({ role: 'listitem', label: tr(`imager.phase.${p}`) })}>
              <View style={{ width: 18, height: 18, borderRadius: 9, alignItems: 'center', justifyContent: 'center', backgroundColor: done ? '#30d158' : here ? t.accent : t.control }}>
                <Text style={{ fontSize: 10, fontWeight: 700, color: done || here ? '#fff' : t.textDim }}>{done ? '✓' : here ? '•' : ''}</Text>
              </View>
              <Text style={{ flex: 1, fontSize: typeScale.body, color: done || here ? t.text : t.textDim, fontWeight: here ? 600 : 400 }}>{tr(`imager.phase.${p}`)}</Text>
              {p === 'download' && done && !here ? <Badge tone="dim">{tr('imager.write.cachedFor')}</Badge> : null}
            </View>
          );
        })}
      </Group>
      {failed && msg ? <Notice tone="bad" title={tr('imager.write.why')} style={{ marginTop: spacing.l }}>{/^imager\.err\./.test(msg) ? tr(msg) : msg}</Notice> : null}
      <View style={{ flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end', marginTop: spacing.xl }}>
        {failed ? (
          <>
            <Button label={tr('imager.back')} onClick={() => set({ job: null })} />
            <Button primary label={tr('imager.write.retry')} onClick={() => run()} />
          </>
        ) : (
          <Button label={tr(j.cancel ? 'imager.write.cancelling' : 'imager.write.cancel')} disabled={j.cancel || j.phase === 'firstrun' || j.phase === 'eject'} onClick={() => set({ dialog: 'cancel' })} />
        )}
      </View>
    </View>
  );
}

function Dialog({ title, text, ok, okRed, onOk, onCancel, cancelLabel }) {
  const t = useTheme();
  return (
    <View onClick={onCancel} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.3)', alignItems: 'center', justifyContent: 'center' }}>
      <View onClick={(e) => e.stopPropagation?.()} {...platform.a11y({ role: 'alertdialog', label: title })}
        style={{ width: 400, padding: 20, gap: 8, borderRadius: t.groupRadius + 2, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 14px 36px rgba(0,0,0,0.45)' }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text }}>{title}</Text>
        <Text style={{ fontSize: typeScale.body, color: t.textDim, lineHeight: 1.45 }}>{text}</Text>
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 12 }}>
          <Button label={cancelLabel} onClick={onCancel} />
          <Button primary label={ok} onClick={onOk} style={okRed ? { backgroundColor: '#e5484d' } : null} />
        </View>
      </View>
    </View>
  );
}

export function WriteStep({ s }) {
  return <View>{s.job ? <Progress s={s} /> : <Review s={s} />}</View>;
}

// drawn by the app's root, over the whole window
export function WriteDialogs({ s }) {
  const { t: tr } = useLocale();
  if (s.dialog === 'confirm' && s.drive) {
    return (
      <Dialog title={tr('imager.confirm.title', { name: s.drive.model })} text={tr('imager.confirm.text', { size: formatSize(s.drive.size), dev: s.drive.name })}
        ok={tr('imager.confirm.ok')} okRed cancelLabel={tr('imager.confirm.cancel')} onCancel={() => set({ dialog: null })} onOk={() => run()} />
    );
  }
  if (s.dialog === 'cancel') {
    return (
      <Dialog title={tr('imager.cancelAsk.title')} text={tr('imager.cancelAsk.text')} ok={tr('imager.cancelAsk.ok')} okRed cancelLabel={tr('imager.cancelAsk.keep')}
        onCancel={() => set({ dialog: null })} onOk={() => { set({ dialog: null }); cancel(); }} />
    );
  }
  return null;
}
