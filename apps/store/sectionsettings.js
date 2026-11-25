// sectionsettings.js — a package system's "Settings" page in the Store (2026-11-23): the second column's item under
// Installed, where what used to sit above each shop now lives (what the system is, its switches, turning it off, the
// lists' last check), so the shop opens straight on its content. The pieces here only draw; each system's page is its
// own module's (FlathubSettings in flathubpages.js, WebSettings in webapps.js, DebianSettings and LordOSSettings here).
import { createElement as h, useState } from 'react';
import { View, Text, Button, Toggle, GroupBox, useTheme, useStore, admin, formatDate, updatePrefs, setUpdatePrefs, typeScale, spacing, t } from '@lordos/sdk';
import { debian } from './debian.js';

// The page: a column of groups, no wider than a settings pane.
export const SettingsPage = ({ children }) => h(View, { style: { gap: spacing.xl, maxWidth: 720 } }, ...[].concat(children));

// What the system is: a paragraph in the dimmed text.
export function About({ children }) {
  const th = useTheme();
  return h(Text, { style: { fontSize: typeScale.body, color: th.textDim, lineHeight: 1.45, textAlign: 'start' } }, children);
}

// A group with an optional heading over an inset box of rows.
export function Group({ title, children }) {
  const th = useTheme();
  return h(View, null,
    title ? h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, paddingHorizontal: spacing.xs, marginBottom: 6, textAlign: 'start' } }, title) : null,
    h(GroupBox, null, ...[].concat(children)));
}

// A row: a label (and a hint under it) and, at the end, its control (a switch or a button).
export function Row({ label, hint, control }) {
  const th = useTheme();
  return h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: spacing.m, minHeight: 44, paddingVertical: 8 } },
    h(View, { style: { flex: 1, minWidth: 0, gap: 2 } },
      h(Text, { style: { fontSize: typeScale.body, color: th.text, lineHeight: 1.35, textAlign: 'start' } }, label),
      hint ? h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.4, textAlign: 'start' } }, hint) : null),
    control || null);
}

// The package lists (Debian's and LordOS's repository, one index): when they were last checked, Check now, and the
// automatic check (the same preference as Store → Updates).
function ListsGroup() {
  const { job } = useStore(debian.state);
  const [prefs, setPrefs] = useState(updatePrefs);
  let at = 0;
  try { at = debian.available() ? lord.pkg.lastUpdate() : 0; } catch (e) { at = 0; }
  const auto = (on) => { setUpdatePrefs({ autoCheck: on }); setPrefs(updatePrefs()); };
  return h(Group, { title: t('store.settings.listsTitle') },
    h(Row, { label: t('store.settings.listsHint'), hint: at ? t('store.debian.lastUpdate', { date: formatDate(new Date(at * 1000), 'short') }) : null,
      control: h(Button, { small: true, label: t('store.debian.update'), disabled: !!job || !admin.available(), onClick: () => !job && refreshLists() }) }),
    h(Row, { label: t('store.updates.auto'), hint: t('store.updates.autoHint'), control: h(Toggle, { on: prefs.autoCheck, onChange: auto }) }));
}

// Fetches the package lists again (the title bar's update icon of Debian's and LordOS's sections, and the button above).
export const refreshLists = () => debian.runJob('pkg-update', '', t('store.job.update'));

// GNU/Linux apps (Debian) → Settings.
export function DebianSettings() {
  return h(SettingsPage, null,
    h(About, null, t('store.settings.debianAbout')),
    h(debian.Progress),
    h(ListsGroup));
}

// LordOS apps → Settings: what LordOS's own items and the .lpk packages are, and the package lists their items come
// from (LordOS's repository).
export function LordOSSettings() {
  return h(SettingsPage, null,
    h(About, null, t('store.lordos.intro')),
    h(About, null, t('store.lordos.subtitle')),
    h(debian.Progress),
    h(ListsGroup));
}
