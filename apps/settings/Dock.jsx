// Settings → Desktop & Dock: Dock position (bottom, left, right), size, magnification, hiding (never,
// only while a window covers it, always: issue #46) and recent apps; in the Classic theme, the taskbar
// position. Saved in settings.json (sdk/dock.js: DOCK_DEFAULTS) and the theme applies it live, including
// the work area. «Reset Dock to defaults» puts the theme's apps back (dock.reset), after a confirmation in the page.
// Menu Bar (Taskbar in Lord Classic) and Control Center: what the bar and the panel show (issue #58,
// sdk/menubarmodel.js); everything is on until the user turns it off. Title bars: the side of the window buttons in
// both themes (sdk/windowbuttons.js, issue #58).
import {
  View, Text, Button, useTheme, useLocale, useSettings, Toggle, Slider, Segmented, dockPrefs, dockHideSettings, DOCK_HIDE_MODES, DOCK_SIZE_MIN, DOCK_SIZE_MAX, DOCK_MAGNIFY_MAX, dock, themes,
  spacing, typeScale, menuBarPrefs, menuBarPatch, cleanerItemShown, getApp, menuBarEveryDisplay, WINDOW_BUTTON_SIDES, windowButtonsPref,
} from '@lordos/sdk';
import { useState } from 'react';
import { Stack, Section, SettingRow, Actions } from './ui.jsx';
import { DesktopsSection } from './Desktops.jsx';

// A small picture of the screen with the Dock (or the taskbar) on the chosen side.
function Preview({ side, t }) {
  const W = 96, H = 60, k = 6;
  const bar = side === 'bottom' ? { left: 24, right: 24, bottom: 3, height: k }
    : side === 'top' ? { left: 0, right: 0, top: 0, height: k }
      : side === 'left' ? { left: 3, top: 14, bottom: 10, width: k } : { right: 3, top: 14, bottom: 10, width: k };
  return (
    <View style={{ width: W, height: H, borderRadius: 7, overflow: 'hidden', background: 'linear-gradient(135deg, #4b3aa8, #c0457a)',
      border: `1px solid ${t.separator}`, direction: 'ltr' }}>
      <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 4, backgroundColor: 'rgba(0,0,0,0.35)' }} />
      <View style={{ position: 'absolute', ...bar, borderRadius: 3, backgroundColor: 'rgba(255,255,255,0.8)' }} />
    </View>
  );
}

// The sides as picture tiles (like Appearance), the chosen one ringed in the accent color.
function SidePicker({ value, sides, onChange }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  return (
    <View style={{ flexDirection: 'row', flexWrap: 'wrap', justifyContent: 'center', gap: spacing.l, paddingVertical: spacing.m }}>
      {sides.map((v) => (
        <View key={v} onClick={() => onChange(v)} style={{ alignItems: 'center', gap: 6 }}>
          <View style={{ padding: 2, borderRadius: 9, border: `2px solid ${value === v ? t.accent : 'transparent'}` }}>
            <Preview side={v} t={t} />
          </View>
          <Text style={{ fontSize: typeScale.callout, fontWeight: value === v ? 600 : 400, color: t.text }}>{tr(`settings.dock.${v}`)}</Text>
        </View>
      ))}
    </View>
  );
}

function SizeSlider({ value, min, max, onChange }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const cap = { fontSize: typeScale.caption, color: t.textDim };
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
      <Text style={cap}>{tr('settings.dock.small')}</Text>
      <Slider value={value} min={min} max={max} width={150} onChange={(v) => onChange(Math.round(v))} />
      <Text style={cap}>{tr('settings.dock.large')}</Text>
    </View>
  );
}

// The pinned apps back to the theme's: the button asks first, in place (never alert()).
function ResetDock({ classic }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [ask, setAsk] = useState(false);
  const [done, setDone] = useState(false);
  return (
    <Section footer={tr(classic ? 'settings.dock.resetHintTaskbar' : 'settings.dock.resetHint')}>
      {ask ? (
        <View style={{ gap: spacing.s, paddingVertical: spacing.s }}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('settings.dock.resetAsk')}</Text>
          <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('settings.dock.resetDetail')}</Text>
          <Actions>
            <Button label={tr('settings.dock.resetCancel')} onClick={() => setAsk(false)} />
            <Button primary label={tr('settings.dock.resetConfirm')} onClick={() => { dock.reset(); setAsk(false); setDone(true); }} />
          </Actions>
        </View>
      ) : (
        <SettingRow label={tr(classic ? 'settings.dock.resetTaskbar' : 'settings.dock.reset')} hint={done ? tr('settings.dock.resetDone') : undefined}>
          <Button label={tr('settings.dock.resetButton')} onClick={() => { setDone(false); setAsk(true); }} />
        </SettingRow>
      )}
    </Section>
  );
}

// What the bar shows next to the clock, in both themes. The Cleaner item (the crown) shares its key with
// Cleaner → Settings.
function MenuBarSection({ classic }) {
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const p = menuBarPrefs(s);
  // text: the row's own text keys when they differ from the setting's (the taskbar's wording in Lord Classic)
  const row = (key, on, hint, text = key) => (
    <SettingRow key={key} label={tr(`settings.dock.menubar.${text}`)} hint={hint ? tr(`settings.dock.menubar.${text}Hint`) : undefined}>
      <Toggle small on={on} onChange={(v) => set(menuBarPatch(s, key, v))} />
    </SettingRow>
  );
  return (
    <Section title={tr(classic ? 'settings.dock.menubar.titleTaskbar' : 'settings.dock.menubar.title')} footer={tr('settings.dock.menubar.footer')}>
      {/* a full bar on each display: Lord Flow's menu bar, Lord Classic's taskbar (the same setting) */}
      {row('everyDisplay', menuBarEveryDisplay(s), true, classic ? 'everyDisplayTaskbar' : 'everyDisplay')}
      {getApp('cleaner') ? row('cleaner', cleanerItemShown(s), true) : null}
      {row('keyboard', p.keyboard, true)}
      {row('osk', p.osk, true)}
      {row('bell', p.bell, true)}
      {row('calendar', p.calendar, true)}
    </Section>
  );
}

// Lord Flow's Control Center: the tiles that can go. Dark mode, transparency, sound and the wallpaper stay.
function ControlCenterSection() {
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const p = menuBarPrefs(s);
  const row = (key, hint) => (    // ccSystem → settings.dock.cc.system
    <SettingRow key={key} label={tr(`settings.dock.cc.${key.slice(2).toLowerCase()}`)} hint={hint ? tr(`settings.dock.cc.${key.slice(2).toLowerCase()}Hint`) : undefined}>
      <Toggle small on={p[key]} onChange={(v) => set(menuBarPatch(s, key, v))} />
    </SettingRow>
  );
  return (
    <Section title={tr('settings.dock.cc.title')} footer={tr('settings.dock.cc.footer')}>
      {row('ccSystem', true)}
      {row('ccNetwork')}
      {row('ccWifi', true)}
      {row('ccBluetooth', true)}
    </Section>
  );
}

// Where the close, minimize and maximize buttons go: the theme's own side, the left or the right, in Lord Flow and
// Lord Classic alike (and for Linux apps that draw their own title bar).
function WindowsSection({ classic }) {
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  return (
    <Section title={tr('settings.dock.windows.title')} footer={tr(classic ? 'settings.dock.windows.footerClassic' : 'settings.dock.windows.footer')}>
      <SettingRow label={tr('settings.dock.windows.buttons')}>
        <Segmented small appearance="neutral" value={windowButtonsPref(s)} onChange={(windowButtons) => set({ windowButtons })}
          options={WINDOW_BUTTON_SIDES.map((v) => [v, tr(`settings.dock.windows.${v}`)])} />
      </SettingRow>
    </Section>
  );
}

export function DockSettings() {
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const p = dockPrefs(s);
  const classic = themes.current() === 'classic';
  if (classic) {
    return (
      <Stack>
        <Section title={tr('settings.dock.taskbar')} footer={`${tr('settings.dock.taskbarHint')} ${tr('settings.dock.sidesHint')}`}>
          <SidePicker value={p.taskbarPosition} sides={['bottom', 'top', 'left', 'right']} onChange={(v) => set({ taskbarPosition: v })} />
        </Section>
        {p.taskbarPosition === 'bottom' || p.taskbarPosition === 'top' ? (
          <Section>
            <SettingRow label={tr('settings.dock.taskbarAlign')}>
              <Segmented small appearance="neutral" value={s.taskbarAlign === 'start' ? 'start' : 'center'} onChange={(taskbarAlign) => set({ taskbarAlign })}
                options={[['center', tr('settings.dock.alignCenter')], ['start', tr('settings.dock.alignStart')]]} />
            </SettingRow>
          </Section>
        ) : null}
        <MenuBarSection classic />
        <WindowsSection classic />
        <ResetDock classic />
      </Stack>
    );
  }
  return (
    <Stack>
      <DesktopsSection tr={tr} />
      <Section title={tr('settings.dock.position')} footer={tr('settings.dock.sidesHint')}>
        <SidePicker value={p.dockPosition} sides={['left', 'bottom', 'right']} onChange={(v) => set({ dockPosition: v })} />
      </Section>
      <Section>
        <SettingRow label={tr('settings.dock.size')}>
          <SizeSlider value={p.dockSize} min={DOCK_SIZE_MIN} max={DOCK_SIZE_MAX} onChange={(dockSize) => set({ dockSize })} />
        </SettingRow>
        <SettingRow label={tr('settings.dock.magnify')} hint={tr('settings.dock.magnifyHint')}>
          <Toggle small on={p.dockMagnify} onChange={(dockMagnify) => set({ dockMagnify })} />
        </SettingRow>
        {p.dockMagnify && (
          <SettingRow label={tr('settings.dock.magnifySize')}>
            <SizeSlider value={p.dockMagnifySize} min={p.dockSize} max={DOCK_MAGNIFY_MAX} onChange={(dockMagnifySize) => set({ dockMagnifySize })} />
          </SettingRow>
        )}
      </Section>
      <Section>
        <SettingRow label={tr('settings.dock.autohide')} hint={tr(`settings.dock.hideHint.${p.dockHide}`)}>
          <Segmented small appearance="neutral" value={p.dockHide} onChange={(mode) => set(dockHideSettings(mode))}
            options={DOCK_HIDE_MODES.map((m) => [m, tr(`settings.dock.hide.${m}`)])} />
        </SettingRow>
        <SettingRow label={tr('settings.dock.recents')} hint={tr('settings.dock.recentsHint')}>
          <Toggle small on={p.dockRecents} onChange={(dockRecents) => set({ dockRecents })} />
        </SettingRow>
      </Section>
      <MenuBarSection />
      <ControlCenterSection />
      <WindowsSection />
      <ResetDock />
    </Stack>
  );
}
