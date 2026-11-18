// The activity editor, in two steps: 1. the name, the icon and the colour; 2. the apps pinned to it and the layout of
// the main display. It is the first-run onboarding (inside the empty group frame of the only activity, until it is set
// up: "Start activity" opens the pinned apps, which the layout tiles) and the "Icon, colour and apps…" dialog of a pill.
import { View, Text, TextInput, ScrollView, useLauncherApps, compareNames, desktopName, t as tr } from '@lordos/sdk';
import { useMemo, useState } from 'react';
import { Surface } from './Surface.jsx';
import { ActivityIcon, LayoutMini, LAYOUT_CELLS } from '../glyphs.jsx';
import { ACTIVITY_ICONS, ACTIVITY_COLORS, MAX_PINS, togglePin } from '../model/activities.js';
import { LAYOUTS } from '../model/layouts.js';

function Button({ t, label, primary, onClick, disabled }) {
  return (
    <View onClick={disabled ? undefined : onClick} style={{ height: 34, paddingHorizontal: 16, borderRadius: 10, justifyContent: 'center', opacity: disabled ? 0.5 : 1,
      ...(primary ? { background: t.brandGradient } : { backgroundColor: t.hover, border: `1px solid ${t.separator}` }) }}>
      <Text style={{ fontSize: 12.5, fontWeight: 600, color: primary ? '#ffffff' : t.text, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}

function Steps({ t, step }) {
  return (
    <View style={{ flexDirection: 'row', gap: 6 }}>
      {[1, 2].map((i) => <View key={i} style={{ width: i === step ? 22 : 8, height: 8, borderRadius: 4, backgroundColor: i === step ? t.accent : t.separator }} />)}
    </View>
  );
}

// id: the desktop; meta: its activity; layout: the main display's; onDone({ name, icon, color, pins, layout }); onCancel
// (none for the onboarding: it can be skipped instead, onSkip)
export function ActivityEditor({ t, id, meta, layout: layout0, onboarding, onDone, onCancel, onSkip, style }) {
  const apps = useLauncherApps();
  const [step, setStep] = useState(1);
  const [name, setName] = useState(onboarding ? '' : desktopName(id));
  const [icon, setIcon] = useState(meta.icon);
  const [color, setColor] = useState(meta.color);
  const [pins, setPins] = useState(meta.pins);
  const [layout, setLayout] = useState(layout0 && layout0 !== 'free' ? layout0 : onboarding ? 'split' : layout0 || 'free');
  const sorted = useMemo(() => [...apps].sort((a, b) => compareNames(a.name, b.name)), [apps]);
  const done = () => onDone({ name: name.trim(), icon, color, pins, layout });
  return (
    <Surface t={t} radius={22} style={{ width: 560, padding: 22, gap: 14, backgroundColor: t.panelBg, ...style }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12 }}>
        <View style={{ width: 44, height: 44, borderRadius: 13, alignItems: 'center', justifyContent: 'center', backgroundColor: color }}>
          <ActivityIcon icon={icon} color="#ffffff" bg={color} size={22} />
        </View>
        <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
          <Text style={{ fontSize: 17, fontWeight: 700, color: t.text, textAlign: 'start' }}>
            {tr(onboarding ? (step === 1 ? 'shell.ws.onboard.title1' : 'shell.ws.onboard.title2') : 'shell.ws.activity.editTitle')}</Text>
          <Text style={{ fontSize: 12.5, color: t.textDim, textAlign: 'start' }}>{tr(step === 1 ? 'shell.ws.onboard.body1' : 'shell.ws.onboard.body2')}</Text>
        </View>
        <Steps t={t} step={step} />
      </View>
      {step === 1 ? (
        <View style={{ gap: 12 }}>
          <TextInput value={name} onChange={setName} autoFocus placeholder={tr('shell.ws.onboard.namePlaceholder')} onSubmit={() => setStep(2)} style={{ height: 34 }} />
          <View style={{ gap: 6 }}>
            <Text style={{ fontSize: 11.5, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{tr('shell.ws.onboard.icon')}</Text>
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6 }}>
              {ACTIVITY_ICONS.map((x) => (
                <View key={x} onClick={() => setIcon(x)} style={{ width: 40, height: 40, borderRadius: 11, alignItems: 'center', justifyContent: 'center',
                  backgroundColor: x === icon ? color : t.hover, border: `1px solid ${x === icon ? color : t.separator}` }}>
                  <ActivityIcon icon={x} color={x === icon ? '#ffffff' : t.text} bg={x === icon ? color : t.solidBg} size={18} />
                </View>
              ))}
            </View>
          </View>
          <View style={{ gap: 6 }}>
            <Text style={{ fontSize: 11.5, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{tr('shell.ws.onboard.color')}</Text>
            <View style={{ flexDirection: 'row', gap: 8, direction: 'ltr' }}>
              {ACTIVITY_COLORS.map((c) => (
                <View key={c} onClick={() => setColor(c)} style={{ width: 26, height: 26, borderRadius: 13, backgroundColor: c, border: c === color ? `2px solid ${t.text}` : '0 solid transparent' }} />
              ))}
            </View>
          </View>
        </View>
      ) : (
        <View style={{ gap: 12 }}>
          <View style={{ gap: 6 }}>
            <Text style={{ fontSize: 11.5, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{tr('shell.ws.onboard.pins', { n: pins.length, max: MAX_PINS })}</Text>
            <ScrollView style={{ height: 196 }}>
              <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 4 }}>
                {sorted.map((a) => {
                  const on = pins.includes(a.id);
                  return (
                    <View key={a.id} onClick={() => setPins((p) => togglePin(p, a.id))}
                      style={{ width: 100, height: 76, borderRadius: 11, alignItems: 'center', justifyContent: 'center', gap: 5, paddingHorizontal: 4,
                        backgroundColor: on ? t.hover : 'transparent', border: `1.5px solid ${on ? t.accent : 'transparent'}` }}>
                      <a.Icon size={30} />
                      <Text style={{ fontSize: 11, color: t.text, textAlign: 'center', numberOfLines: 1, maxWidth: 92 }}>{a.name}</Text>
                    </View>
                  );
                })}
              </View>
            </ScrollView>
          </View>
          <View style={{ gap: 6 }}>
            <Text style={{ fontSize: 11.5, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{tr('shell.ws.layout.title')}</Text>
            <View style={{ flexDirection: 'row', gap: 6 }}>
              {LAYOUTS.map((l) => (
                <View key={l} onClick={() => setLayout(l)} title={tr(`shell.ws.layout.${l}`)}
                  style={{ width: 54, height: 40, borderRadius: 9, alignItems: 'center', justifyContent: 'center', backgroundColor: l === layout ? t.accent : t.hover }}>
                  <LayoutMini cells={LAYOUT_CELLS[l]} w={30} h={20} color={l === layout ? '#ffffff' : t.text} fill={l === layout ? '#ffffff' : t.text} active />
                </View>
              ))}
              <Text style={{ fontSize: 12, color: t.text, alignSelf: 'center', marginStart: 6 }}>{tr(`shell.ws.layout.${layout}`)}</Text>
            </View>
          </View>
        </View>
      )}
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        {onboarding && onSkip ? <View onClick={onSkip}><Text style={{ fontSize: 12, color: t.textDim }}>{tr('shell.ws.onboard.skip')}</Text></View> : null}
        <View style={{ flex: 1 }} />
        {step === 2 ? <Button t={t} label={tr('shell.ws.onboard.back')} onClick={() => setStep(1)} /> : onCancel ? <Button t={t} label={tr('shell.ws.onboard.cancel')} onClick={onCancel} /> : null}
        {step === 1 ? <Button t={t} primary label={tr('shell.ws.onboard.next')} onClick={() => setStep(2)} />
          : <Button t={t} primary label={tr(onboarding ? 'shell.ws.onboard.start' : 'shell.ws.onboard.save')} onClick={done} />}
      </View>
    </Surface>
  );
}
