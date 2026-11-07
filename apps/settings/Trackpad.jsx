// Settings → Mouse & Trackpad, one section per device: Pointer (size, for every device), Mouse
// (speed, natural scrolling, primary button) and Trackpad (speed, touch sensitivity, tap to click,
// secondary click, three-finger drag, scrolling and its speed, inertia, pinch and swipe). Each
// trackpad row comes with a small drawing of the gesture that animates on mouse hover. Saved in
// settings.trackpad and applied live (sdk/trackpad.js → lord.input.configure). See docs/TRACKPAD.md.
import { View, Text, useTheme, useLocale, Toggle, Segmented, Slider, useSettings, useTrackpads, TRACKPAD_DEFAULTS, setTrackpad, spacing, typeScale } from '@lordos/sdk';
import { Stack, Section, SettingRow, Note } from './ui.jsx';
import { useEffect, useState } from 'react';

const PAD_W = 64, PAD_H = 44;
const POINTER_SIZES = [0.75, 1, 1.25, 1.5, 2];

// Finger positions (0..1 inside the touchpad) according to the gesture phase p (0..1).
function fingers(kind, p) {
  const e = p < 0.5 ? p * 2 : 2 - p * 2;          // there and back
  const row = (n, y, x0, dx = 0) => Array.from({ length: n }, (_, i) => [x0 + i * 0.16 + dx, y]);
  switch (kind) {
    case 'speed': return [[0.25 + 0.5 * e, 0.5]];
    case 'tap': return [[0.5, 0.5, p < 0.25]];
    case 'tapDrag': return p < 0.2 ? [[0.3, 0.55, true]] : [[0.3 + 0.4 * Math.min(1, (p - 0.2) * 1.6), 0.55]];
    case 'secondary': return [[0.42, 0.5, p < 0.25], [0.58, 0.5, p < 0.25]];
    case 'threeDrag': return row(3, 0.55, 0.2, 0.3 * e);
    case 'dwt': return [[0.5, 0.5]];
    case 'sensitivity': return [[0.5, 0.5, p < 0.5]];
    case 'scrollSpeed': return [[0.42, 0.8 - 0.6 * e], [0.58, 0.8 - 0.6 * e]];
    case 'natural':
    case 'scroll': return [[0.42, 0.75 - 0.5 * e], [0.58, 0.75 - 0.5 * e]];
    case 'kinetic': return p < 0.35 ? [[0.42, 0.8 - p * 1.5], [0.58, 0.8 - p * 1.5]] : [];
    case 'pinch': return [[0.38 - 0.18 * e, 0.62 + 0.14 * e], [0.62 + 0.18 * e, 0.38 - 0.14 * e]];
    case 'swipe3': return row(3, 0.5, 0.5 - 0.3 * e);
    case 'swipe4': return row(4, 0.75 - 0.45 * e, 0.26);
  }
  return [];
}

function GestureDemo({ kind, active }) {
  const t = useTheme();
  const [p, setP] = useState(0.35);
  useEffect(() => {
    if (!active) { setP(0.35); return undefined; }
    const t0 = lord.now();
    const id = setInterval(() => setP(((lord.now() - t0) / 1600) % 1), 50);
    return () => clearInterval(id);
  }, [active]);
  // natural: a "page" that moves up with the fingers; inertia: keeps going by itself for a while afterwards
  const page = kind === 'natural' || kind === 'kinetic';
  const pageY = kind === 'natural' ? 0.75 - 0.5 * (p < 0.5 ? p * 2 : 2 - p * 2) : 0.8 - Math.min(1, 1 - Math.pow(1 - p, 3)) * 0.65;
  return (
    <View style={{ width: PAD_W, height: PAD_H, borderRadius: 7, backgroundColor: t.control, border: `1px solid ${t.separator}`, overflow: 'hidden', direction: 'ltr' }}>
      {page && <View style={{ position: 'absolute', left: PAD_W * 0.78, top: PAD_H * pageY - 6, width: 6, height: 12, borderRadius: 2, backgroundColor: t.textDim, opacity: 0.6 }} />}
      {kind === 'dwt' && <Text style={{ position: 'absolute', left: 6, top: 4, fontSize: 11, color: t.textDim }}>⌨</Text>}
      {fingers(kind, p).map(([x, y, press], i) => (
        <View key={i} style={{ position: 'absolute', left: x * PAD_W - 5, top: y * PAD_H - 5, width: 10, height: 10, borderRadius: 5,
          backgroundColor: t.accent, opacity: kind === 'dwt' ? 0.35 : press ? 1 : 0.8, border: press ? '2px solid #fff' : '0 solid transparent' }} />
      ))}
    </View>
  );
}

// A row of the page: the gesture drawing, the label and hint, and the control at the end. The drawing
// plays while the pointer is over the row.
function Row({ kind, label, hint, children, stack }) {
  const [hover, setHover] = useState(false);
  return (
    <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}>
      <SettingRow icon={<GestureDemo kind={kind} active={hover} />} label={label} hint={hint} minHeight={56} stack={stack}>{children}</SettingRow>
    </View>
  );
}

export function TrackpadSettings() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [s] = useSettings();
  const devices = useTrackpads().filter((d) => d.kind === 'touchpad');
  const [tab, setTab] = useState('point');
  const c = { ...TRACKPAD_DEFAULTS, ...(s.trackpad || {}) };
  const toggle = (key) => <Toggle small on={!!c[key]} onChange={(v) => setTrackpad({ [key]: v })} />;
  const slider = (key, lo = 'trackpad.slow', hi = 'trackpad.fast') => (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
      <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr(lo)}</Text>
      <Slider value={c[key]} onChange={(v) => setTrackpad({ [key]: Math.round(v * 100) / 100 })} width={120} />
      <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr(hi)}</Text>
    </View>
  );
  // the nearest of the sizes offered (an older or hand-edited value still selects one)
  const size = POINTER_SIZES.reduce((a, b) => (Math.abs(b - c.pointerSize) < Math.abs(a - c.pointerSize) ? b : a), 1);
  return (
    <Stack>
      <Section title={tr('trackpad.pointer.title')}>
        <SettingRow label={tr('trackpad.pointer.size')} hint={tr('trackpad.pointer.sizeHint')} stack>
          <Segmented small value={size} onChange={(v) => setTrackpad({ pointerSize: v })}
            options={[[0.75, tr('trackpad.pointer.small')], [1, tr('trackpad.pointer.default')], [1.25, tr('trackpad.pointer.large')],
              [1.5, tr('trackpad.pointer.larger')], [2, tr('trackpad.pointer.largest')]]} />
        </SettingRow>
        <SettingRow label={tr('trackpad.pointer.shake')} hint={tr('trackpad.pointer.shakeHint')}>{toggle('shakeToLocate')}</SettingRow>
      </Section>
      <Section title={tr('trackpad.mouse.title')}>
        <SettingRow label={tr('trackpad.mouse.speed')} stack={false}>{slider('mouseSpeed')}</SettingRow>
        <SettingRow label={tr('trackpad.mouse.natural')} hint={tr('trackpad.mouse.naturalHint')}>{toggle('mouseNatural')}</SettingRow>
        <SettingRow label={tr('trackpad.mouse.primary')} hint={tr('trackpad.mouse.primaryHint')}>
          <Segmented small value={c.mouseLeftHanded ? 'right' : 'left'} onChange={(v) => setTrackpad({ mouseLeftHanded: v === 'right' })}
            options={[['left', tr('trackpad.mouse.primary.left')], ['right', tr('trackpad.mouse.primary.right')]]} />
        </SettingRow>
      </Section>
      <Section title={tr('trackpad.pad.title')} />
      {devices.length === 0 && <Note>{tr('trackpad.noDevice')}</Note>}
      <View style={{ alignItems: 'center' }}>
        <Segmented appearance="neutral" value={tab} onChange={setTab} options={[['point', tr('trackpad.tab.point')], ['scroll', tr('trackpad.tab.scroll')], ['more', tr('trackpad.tab.more')]]} />
      </View>
      {tab === 'point' && (
        <Section>
          <Row kind="speed" label={tr('trackpad.speed')} hint={tr('trackpad.speedHint')}>{slider('speed')}</Row>
          <Row kind="sensitivity" label={tr('trackpad.sensitivity')} hint={tr('trackpad.sensitivityHint')} stack>
            <Segmented small value={c.sensitivity} onChange={(v) => setTrackpad({ sensitivity: v })}
              options={[['low', tr('trackpad.sensitivity.low')], ['medium', tr('trackpad.sensitivity.medium')], ['high', tr('trackpad.sensitivity.high')]]} />
          </Row>
          <Row kind="tap" label={tr('trackpad.tap')} hint={tr('trackpad.tapHint')}>{toggle('tap')}</Row>
          <Row kind="tapDrag" label={tr('trackpad.tapDrag')} hint={tr('trackpad.tapDragHint')}>{toggle('tapDrag')}</Row>
          <Row kind="secondary" label={tr('trackpad.secondary')} hint={tr('trackpad.secondaryHint')} stack>
            <Segmented small value={c.secondary} onChange={(v) => setTrackpad({ secondary: v })}
              options={[['two-fingers', tr('trackpad.secondary.two')], ['bottom-right', tr('trackpad.secondary.right')], ['bottom-left', tr('trackpad.secondary.left')]]} />
          </Row>
          <Row kind="threeDrag" label={tr('trackpad.threeDrag')} hint={tr('trackpad.threeDragHint')}>{toggle('threeDrag')}</Row>
          <Row kind="dwt" label={tr('trackpad.dwt')} hint={tr('trackpad.dwtHint')}>{toggle('dwt')}</Row>
        </Section>
      )}
      {tab === 'scroll' && (
        <Section>
          <Row kind="natural" label={tr('trackpad.natural')} hint={tr('trackpad.naturalHint')}>{toggle('natural')}</Row>
          <Row kind="scroll" label={tr('trackpad.scroll')} hint={tr('trackpad.scrollHint')}>{toggle('scroll')}</Row>
          <Row kind="scrollSpeed" label={tr('trackpad.scrollSpeed')} hint={tr('trackpad.scrollSpeedHint')}>{slider('scrollSpeed')}</Row>
          <Row kind="kinetic" label={tr('trackpad.kinetic')} hint={tr('trackpad.kineticHint')}>{toggle('kinetic')}</Row>
          <Row kind="pinch" label={tr('trackpad.pinch')} hint={tr('trackpad.pinchHint')}>{toggle('pinch')}</Row>
        </Section>
      )}
      {tab === 'more' && (
        <Section>
          <Row kind="swipe3" label={tr(c.threeDrag ? 'trackpad.swipe3Moved' : 'trackpad.swipe3')} hint={tr('trackpad.swipe3Hint')}>{toggle('swipe3')}</Row>
          <Row kind="swipe4" label={tr('trackpad.swipe4')} hint={tr('trackpad.swipe4Hint')}>{toggle('swipe4')}</Row>
        </Section>
      )}
      {devices.map((d, i) => (
        <Note key={i}>{tr('trackpad.device', { name: d.name, w: d.width, h: d.height })}{d.clickpad ? ` · ${tr('trackpad.clickpad')}` : ''}</Note>
      ))}
    </Stack>
  );
}
