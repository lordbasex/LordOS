// Settings → Default Apps: browser, mail, music, video, images, PDF, text editor and terminal, from
// LordOS's own apps and the installed Linux apps (.desktop with MimeType). Saved in
// ~/.config/mimeapps.list (and ~/.config/xdg-terminals.list), so Files' "Open With", double
// clicks, lord.apps.launchWith and Linux apps (xdg-open) agree. See docs/SETTINGS.md.
import { View, Text, useTheme, useLocale, useLauncherApps, DEFAULT_CATEGORIES, defaultInfo, setDefaultFor, resetDefaultFor, candidatesFor } from '@lordos/sdk';
import { useState } from 'react';
import { GroupBox, SettingRow, PopUp, Note, Stack, Badge } from './ui.jsx';

// The name of a default app's desktop id: from the category's candidates, else the installed Linux
// apps, else the id itself.
function nameOf(desktopId, list, apps) {
  const c = list.find((a) => a.id === desktopId);
  if (c) return c.name;
  const a = apps.find((x) => x.kind === 'linux' && `${x.desktopId}.desktop` === desktopId);
  return a ? a.name : desktopId.replace(/\.desktop$/, '');
}

const LOOK = {
  browser: ['◎', '#0a84ff'], mail: ['✉', '#30a0ff'], music: ['♪', '#ff375f'], video: ['▶', '#bf5af2'], images: ['▣', '#ff9f0a'],
  pdf: ['⎙', '#ff453a'], text: ['✎', '#8e8e93'], terminal: ['>_', '#2c2c2e'],
};

export function DefaultAppsSettings() {
  const { t: tr } = useLocale();
  const apps = useLauncherApps();
  const [rev, setRev] = useState(0);
  const [err, setErr] = useState('');
  return (
    <Stack>
      <GroupBox footer={tr('prefs.defaults.footer')}>
        {DEFAULT_CATEGORIES.map((c) => {
          const list = candidatesFor(c.id, apps);
          const info = defaultInfo(c.id);
          const opts = list.map((a) => [a.id, a.name, a.Icon]);
          // the user chose nothing: the app that opens these files now, as "Automatic (name)"; an
          // explicit choice can go back to it (the user's lines leave mimeapps.list)
          const cur = info && !info.auto ? info.id : '';
          if (cur && !opts.some(([id]) => id === cur)) opts.push([cur, tr('prefs.defaults.other', { id: cur.replace(/\.desktop$/, '') })]);
          let first = ['', tr('prefs.defaults.choose')];
          if (info && info.auto) first = ['', tr('prefs.defaults.automatic', { name: nameOf(info.id, list, apps) })];
          else if (cur && !c.terminal) first = ['', tr('prefs.defaults.automaticPlain')];
          const options = cur && c.terminal ? opts : [first, ...opts];
          const [glyph, color] = LOOK[c.id];
          return (
            <SettingRow key={`${c.id}${rev}`} icon={<Badge glyph={glyph} color={color} />} label={tr(`prefs.defaults.cat.${c.id}`)}
              hint={list.length || info ? '' : tr('prefs.defaults.none')}>
              {opts.length || info ? <PopUp width={230} value={cur} options={options}
                onChange={(id) => {
                  const ok = id ? setDefaultFor(c.id, id) : !cur || resetDefaultFor(c.id);
                  setErr(ok ? '' : tr('prefs.defaults.failed'));
                  setRev((r) => r + 1);
                }} /> : null}
            </SettingRow>
          );
        })}
      </GroupBox>
      {err ? <Note tone="warn">{err}</Note> : null}
      <Note>{tr('prefs.defaults.filesHint')}</Note>
    </Stack>
  );
}
