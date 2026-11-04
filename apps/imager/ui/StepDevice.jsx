// Step 1 — the device, in two views: the kind (Raspberry Pi, a PC or a generic arm64 machine from a USB
// stick), then, for a Raspberry Pi, which model (the Pi 5 or the Pi 500: the same image, other tips and
// cooling). Boards LordOS doesn't support yet are listed with their status.
import { View, Text, useTheme, useLocale, typeScale, spacing } from '@lordos/sdk';
import { DEVICES, RAM_ADVICE, PI_MODELS, deviceById, deviceLabel } from '../core/catalog.js';
import { chooseDevice, set } from '../state.js';
import { Card, Badge, Group, SectionTitle, Press } from './kit.jsx';
import { BoardArt, PcArt, KeyboardArt } from './art.jsx';

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

const ModelArt = ({ d, size }) => (d.id === 'rpi500' ? <KeyboardArt size={size} /> : <BoardArt size={size} />);

export function DeviceStep({ s }) {
  const dev = deviceById(s.device);
  return dev && dev.kind === 'pi' && s.devView === 'pi' ? <PiModels s={s} /> : <DeviceKinds s={s} />;
}

// The kinds of device: one Raspberry Pi card (its supported models under it), a PC, generic arm64
function DeviceKinds({ s }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const dev = deviceById(s.device);
  const pcs = DEVICES.filter((d) => d.supported && d.kind !== 'pi');
  const piOn = dev && dev.kind === 'pi';
  return (
    <View>
      <StepHeader title={tr('imager.device.title')} text={tr('imager.device.text')} />
      <View style={{ flexDirection: 'row', gap: spacing.l, flexWrap: 'wrap' }}>
        <Card id="dev-pi" label={tr('imager.device.pi')} selected={piOn} onClick={() => { if (!piOn) chooseDevice(PI_MODELS[0].id); }}
          style={{ width: 250, alignItems: 'center' }}>
          <BoardArt size={130} />
          <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, marginTop: 4 }}>{tr('imager.device.pi')}</Text>
          <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'center', lineHeight: 1.4, alignSelf: 'stretch' }}>{PI_MODELS.map((d) => d.name.replace('Raspberry ', '')).join(' · ')}</Text>
          <Badge tone="ok">{tr('imager.device.piCount', { n: PI_MODELS.length })}</Badge>
        </Card>
        {pcs.map((d) => (
          <Card key={d.id} id={`dev-${d.id}`} label={deviceLabel(d, tr)} selected={s.device === d.id} onClick={() => chooseDevice(d.id)}
            style={{ width: 250, alignItems: 'center' }}>
            <PcArt size={130} />
            <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, marginTop: 4 }}>{deviceLabel(d, tr)}</Text>
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'center', lineHeight: 1.4, alignSelf: 'stretch' }}>{tr(`imager.device.${d.id}.text`)}</Text>
            <Badge tone="ok">{tr('imager.device.liveUsb')}</Badge>
          </Card>
        ))}
      </View>
      {piOn ? (
        <Group style={{ marginTop: spacing.xl }}>
          <View style={{ paddingVertical: 10 }}><Text style={{ fontSize: typeScale.body, color: t.text, lineHeight: 1.45 }}>{tr('imager.device.piNext')}</Text></View>
        </Group>
      ) : (
        <Group style={{ marginTop: spacing.xl }} footer={tr('imager.device.pcFooter')}>
          <View style={{ paddingVertical: 10 }}><Text style={{ fontSize: typeScale.body, color: t.text, lineHeight: 1.45 }}>{tr('imager.device.pcText')}</Text></View>
        </Group>
      )}
    </View>
  );
}

// Which Raspberry Pi: the supported models as cards, what to know about the chosen one, the others
function PiModels({ s }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const other = DEVICES.filter((d) => d.kind === 'pi' && !d.supported);
  return (
    <View>
      <Press id="dev-back" label={tr('imager.device.allDevices')} onClick={() => set({ devView: 'kind' })}
        style={{ alignSelf: 'flex-start', paddingVertical: 2, paddingHorizontal: 4, borderRadius: 5, marginBottom: spacing.s }}>
        <Text style={{ fontSize: typeScale.callout, color: t.accent }}>{`‹ ${tr('imager.device.allDevices')}`}</Text>
      </Press>
      <StepHeader title={tr('imager.device.modelTitle')} text={tr('imager.device.modelText')} />
      <View style={{ flexDirection: 'row', gap: spacing.l, flexWrap: 'wrap' }}>
        {PI_MODELS.map((d) => (
          <Card key={d.id} id={`dev-${d.id}`} label={d.name} selected={s.device === d.id} onClick={() => chooseDevice(d.id)}
            style={{ width: 250, alignItems: 'center' }}>
            <ModelArt d={d} size={130} />
            <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, marginTop: 4 }}>{d.name}</Text>
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'center', lineHeight: 1.4, alignSelf: 'stretch' }}>{tr(`imager.device.${d.id}.text`)}</Text>
            <Badge tone="ok">{tr(d.id === 'rpi500' ? 'imager.device.testedByCommunity' : 'imager.device.supported')}</Badge>
          </Card>
        ))}
      </View>
      {s.device === 'rpi5' ? (
        <Group style={{ marginTop: spacing.xl }} footer={tr('imager.device.ramFooter')}>
          <RamMeter />
        </Group>
      ) : (
        <Group style={{ marginTop: spacing.xl }}>
          <View style={{ paddingVertical: 10 }}><Text style={{ fontSize: typeScale.body, color: t.text, lineHeight: 1.45 }}>{tr(`imager.device.${s.device}.note`)}</Text></View>
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
