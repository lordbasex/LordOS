// Settings → Appearance → Window options: three personal choices about every window, in every theme (GitHub #151, #219). Each
// default is how LordOS always was, so nothing changes for anyone who doesn't choose them; saved in settings.json (new
// keys, no migration) and applied live to the open windows.
//   Window buttons      Normal (the default, each theme's own size) / Large / Extra large: the title bar, its buttons,
//                       their glyphs and their hit areas (sdk/windowbuttons.js), with a small live preview
//   Move and resize with  Off (the default) / ❖ + drag / Alt + drag anywhere in a window (sdk/moddrag.js)
//   Show tiling options in the window menu   on by default (sdk/windowmenu.js, each theme's title bar menu)
import {
  View, Text, useTheme, useLocale, useSettings, Toggle, Segmented, spacing,
  WINDOW_BUTTON_SIZES, windowButtonSizePref, scaledBar, DRAG_MODIFIERS, dragModifierPref, windowMenuTiling,
} from '@lordos/sdk';
import { Section, SettingRow } from './ui.jsx';

const LIGHTS = ['#ff5f57', '#febc2e', '#28c840'];

// A title bar's buttons at the chosen size: three round buttons and a title line, drawn the way the title bar grows.
function ButtonsPreview({ size }) {
  const t = useTheme();
  const px = (n) => scaledBar(n, size);
  return (
    <View style={{ alignItems: 'center', paddingVertical: spacing.m }}>
      <View style={{ width: 220, height: px(28), borderRadius: 8, flexDirection: 'row', alignItems: 'center', gap: px(6), paddingHorizontal: 10,
        backgroundColor: t.titleBg || t.panelBg, border: `1px solid ${t.separator}`, direction: 'ltr' }}>
        {LIGHTS.map((c) => <View key={c} style={{ width: px(12), height: px(12), borderRadius: px(12) / 2, backgroundColor: c }} />)}
        <View style={{ flex: 1, alignItems: 'center' }}>
          <View style={{ width: 70, height: 6, borderRadius: 3, backgroundColor: t.separator }} />
        </View>
      </View>
    </View>
  );
}

export function WindowPrefs() {
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const size = windowButtonSizePref(s);
  return (
    <Section title={tr('settings.windows.title')} footer={tr('settings.dock.drag.footer')}>
      <SettingRow label={tr('settings.dock.windows.size')}>
        <Segmented small appearance="neutral" value={size} onChange={(windowButtonSize) => set({ windowButtonSize })}
          options={WINDOW_BUTTON_SIZES.map((v) => [v, tr(`settings.dock.windows.sizes.${v}`)])} />
      </SettingRow>
      <ButtonsPreview size={size} />
      <SettingRow label={tr('settings.dock.drag.label')}>
        <Segmented small appearance="neutral" value={dragModifierPref(s)} onChange={(windowDragModifier) => set({ windowDragModifier })}
          options={DRAG_MODIFIERS.map((v) => [v, tr(`settings.dock.drag.${v}`)])} />
      </SettingRow>
      <SettingRow label={tr('settings.dock.tiling.label')} hint={tr('settings.dock.tiling.footer')}>
        <Toggle small on={windowMenuTiling(s)} onChange={(windowMenuTiling) => set({ windowMenuTiling })} />
      </SettingRow>
    </Section>
  );
}
