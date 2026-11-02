// Step 1 — the device: the Raspberry Pi 5 (the only supported board), or a PC from a USB stick.
import { View, Text, useTheme, useLocale, typeScale, spacing } from '@lordos/sdk';
import { DEVICES, RAM_ADVICE, deviceLabel } from '../core/catalog.js';
import { chooseDevice } from '../state.js';
import { Card, Badge, Group, SectionTitle } from './kit.jsx';
import { BoardArt, PcArt } from './art.jsx';

export function StepHeader({ title, text }) {
  const t = useTheme();
  return (
    <View style={{ gap: 6, marginBottom: spacing.xl }}>
      <Text style={{ fontSize: typeScale.largeTitle, fontWeight: 600, color: t.text }}>{title}</Text>
      {text ? <Text style={{ fontSize: typeScale.body, color: t.textDim, lineHeight: 1.45, maxWidth: 620 }}>{text}</Text> : null}
    </View>
  );
}

function RamMeter() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const colors = { min: '#ff9f0a', comfortable: '#30d158', max: '#B46CFF' };
  return (
    <View style={{ gap: 8, paddingVertical: 10 }}>
      <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text }}>{tr('imager.device.ramTitle')}</Text>
      <View style={{ flexDirection: 'row', gap: 6 }}>
        {RAM_ADVICE.map((r) => (
          <View key={r.gb} style={{ flex: 1, gap: 4 }}>
            <View style={{ height: 6, borderRadius: 3, backgroundColor: colors[r.level] }} />
            <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text }}>{tr('imager.device.ramGb', { n: r.gb })}</Text>
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35 }}>{tr(`imager.device.ram.${r.level}`)}</Text>
          </View>
        ))}
      </View>
    </View>
  );
}

export function DeviceStep({ s }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const supported = DEVICES.filter((d) => d.supported);
  const other = DEVICES.filter((d) => !d.supported);
  return (
    <View>
      <StepHeader title={tr('imager.device.title')} text={tr('imager.device.text')} />
      <View style={{ flexDirection: 'row', gap: spacing.l, flexWrap: 'wrap' }}>
        {supported.map((d) => (
          <Card key={d.id} id={`dev-${d.id}`} label={deviceLabel(d, tr)} selected={s.device === d.id} onClick={() => chooseDevice(d.id)}
            style={{ width: 250, alignItems: 'center' }}>
            {d.kind === 'pi' ? <BoardArt size={130} /> : <PcArt size={130} />}
            <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, marginTop: 4 }}>{deviceLabel(d, tr)}</Text>
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'center', lineHeight: 1.4, alignSelf: 'stretch' }}>{tr(`imager.device.${d.id}.text`)}</Text>
            <Badge tone="ok">{tr(d.kind === 'pi' ? 'imager.device.supported' : 'imager.device.liveUsb')}</Badge>
          </Card>
        ))}
      </View>
      {s.device === 'rpi5' ? (
        <Group style={{ marginTop: spacing.xl }} footer={tr('imager.device.ramFooter')}>
          <RamMeter />
        </Group>
      ) : (
        <Group style={{ marginTop: spacing.xl }} footer={tr('imager.device.pcFooter')}>
          <View style={{ paddingVertical: 10 }}><Text style={{ fontSize: typeScale.body, color: t.text, lineHeight: 1.45 }}>{tr('imager.device.pcText')}</Text></View>
        </Group>
      )}
      <SectionTitle style={{ marginTop: spacing.xxl, marginBottom: spacing.s, fontSize: typeScale.body }}>{tr('imager.device.otherTitle')}</SectionTitle>
      <Group>
        {other.map((d) => (
          <View key={d.id} style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m, minHeight: 36, paddingVertical: 4 }}>
            <View style={{ opacity: 0.5 }}><BoardArt size={34} /></View>
            <Text style={{ flex: 1, fontSize: typeScale.body, color: t.textDim }}>{d.name}</Text>
            <Badge tone="dim">{tr(d.never ? 'imager.device.never' : 'imager.device.notYet')}</Badge>
          </View>
        ))}
      </Group>
    </View>
  );
}
