// themeswitch.jsx — what the person sees while the desktop style changes live (themes.use, docs/THEMES-GUIDE.md
// "Switching live"). lord-server draws two more frames after lord.themes.use accepted the switch (main.c), so this
// overlay is on screen, dimming the old desktop with a ring and "Changing the desktop style…", while the new theme
// loads behind it; the new theme's first frame replaces it. A switch that can't happen (the verified-code check
// refused the theme, or it threw at its start and lord-server went back) keeps the session in the style it had and
// says so in a notification.
import { createElement as h } from 'react';
import { View, Text, screen, mainDisplay } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { t } from './i18n.js';
import { notify } from './notify.js';
import { Spinner } from './components.jsx';

const switching = createStore(null);       // { name } while a switch is on its way
export const themeSwitching = () => switching.get();

export function showThemeSwitch(name) { switching.set({ name: String(name || '') }); }
export function hideThemeSwitch() { switching.set(null); }

// The notification of a switch that didn't happen. entry: the lord.themes.list entry of the theme asked for.
export function notifyThemeNotApplied(entry) {
  const name = entry?.name || entry?.id || '';
  notify({ app: 'settings', title: t('shell.themeSwitch.failedTitle', { name }),
    body: entry?.needsLordOS ? t('shell.themeSwitch.needsLordOSBody', { version: entry.needsLordOS })
      : t(entry?.failedCode ? 'shell.themeSwitch.refusedBody' : 'shell.themeSwitch.failedBody') });
}

export function ThemeSwitchOverlay() {
  const s = useStore(switching);
  if (!s) return null;
  const d = mainDisplay() || { x: 0, y: 0, w: screen.width, h: screen.height };
  // the whole desktop dimmed (it takes the pointer: nothing under it is clicked while the style changes), the card on
  // the main display
  return h(View, { style: { position: 'absolute', left: 0, top: 0, width: screen.width, height: screen.height, backgroundColor: 'rgba(0,0,0,0.55)' } },
    h(View, { style: { position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h, alignItems: 'center', justifyContent: 'center', pointerEvents: 'none' } },
      h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 20, paddingVertical: 14, borderRadius: 12,
        backgroundColor: 'rgba(24,22,34,0.92)', border: '1px solid rgba(255,255,255,0.14)' } },
        h(Spinner, { size: 20, color: '#ffffff' }),
        h(Text, { style: { fontSize: 14, fontWeight: 600, color: '#f4f4f6', textAlign: 'start', whiteSpace: 'nowrap' } },
          s.name ? t('shell.themeSwitch.changingTo', { name: s.name }) : t('shell.themeSwitch.changing')))));
}
