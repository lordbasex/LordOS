// Settings → Date & Time: set the time automatically (NTP with systemd-timesyncd, closed admin action
// time-ntp) or by hand (time-set), the time zone (chosen from a list: it is never guessed from the
// network, so no IP is sent anywhere), the 12/24-hour clock and the menu bar clock format.
// See docs/SETTINGS.md, "Date & Time".
import {
  View, Text, useTheme, useLocale, useSettings, Toggle, Button, TextInput, ScrollView, SearchField, admin, useTimedate, clockPrefs, menuClock,
  formatDate, formatTime, useClock,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { GroupBox, SettingRow, PopUp, Note, Stack } from './ui.jsx';

const pad = (n) => String(n).padStart(2, '0');
const isoDate = (d) => `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
const offsetText = (sec) => {
  const s = sec < 0 ? '−' : '+', a = Math.abs(sec);
  return `UTC${s}${pad(Math.floor(a / 3600))}:${pad(Math.floor(a / 60) % 60)}`;
};

function ZonePicker({ zone, onPick, tr }) {
  const t = useTheme();
  const [q, setQ] = useState('');
  const [all] = useState(() => lord.time.zones().sort());
  const now = Math.floor(Date.now() / 1000);
  const needle = q.trim().toLowerCase().replace(/ /g, '_');
  const shown = (needle ? all.filter((z) => z.toLowerCase().includes(needle)) : all).slice(0, 400);
  return (
    <View style={{ gap: 8, paddingVertical: 8 }}>
      <SearchField value={q} onChange={setQ} placeholder={tr('prefs.time.searchZone')} autoFocus />
      <View style={{ height: 220, borderRadius: 8, border: `1px solid ${t.separator}` }}>
        <ScrollView style={{ padding: 4 }}>
          {shown.map((z) => {
            const off = lord.time.zoneOffset ? lord.time.zoneOffset(z, now) : null;
            const on = z === zone;
            const parts = z.split('/');
            return (
              <View key={z} onClick={() => onPick(z)} style={{ height: 26, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 8, borderRadius: 5,
                backgroundColor: on ? t.accent : 'transparent' }}>
                <Text style={{ flex: 1, fontSize: 12, color: on ? '#fff' : t.text, whiteSpace: 'nowrap', textAlign: 'start' }}>{parts[parts.length - 1].replace(/_/g, ' ')}</Text>
                <Text style={{ fontSize: 11, color: on ? '#fff' : t.textDim, whiteSpace: 'nowrap' }}>{parts.slice(0, -1).join(' / ').replace(/_/g, ' ')}</Text>
                <Text style={{ width: 84, fontSize: 11, color: on ? '#fff' : t.textDim, textAlign: 'end', direction: 'ltr' }}>{off ? offsetText(off.offset) : ''}</Text>
              </View>
            );
          })}
        </ScrollView>
      </View>
    </View>
  );
}

function ManualTime({ tr, onDone }) {
  const now = new Date();
  const [date, setDate] = useState(isoDate(now));
  const [time, setTime] = useState(`${pad(now.getHours())}:${pad(now.getMinutes())}`);
  const [err, setErr] = useState('');
  const valid = /^\d{4}-\d{2}-\d{2}$/.test(date) && /^\d{2}:\d{2}$/.test(time);
  return (
    <View style={{ paddingVertical: 8, gap: 8 }}>
      <View style={{ flexDirection: 'row', gap: 8, alignItems: 'center', direction: 'ltr' }}>
        <View style={{ width: 130 }}><TextInput value={date} onChange={setDate} placeholder="2026-10-18" /></View>
        <View style={{ width: 80 }}><TextInput value={time} onChange={setTime} placeholder="14:30" /></View>
        <Button small primary label={tr('prefs.time.set')} disabled={!valid}
          onClick={() => admin.run('time-set', `${date} ${time}:00`).then(() => { setErr(''); onDone(); }, (e) => setErr(e.message))} />
      </View>
      {err ? <Note tone="warn">{err}</Note> : <Note>{tr('prefs.time.manualHint')}</Note>}
    </View>
  );
}

export function DateTimeSettings() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const td = useTimedate();
  const c = clockPrefs(s);
  const [zone, setZone] = useState(() => lord.time.zone());
  const [picking, setPicking] = useState(false);
  const [editing, setEditing] = useState(false);
  const [err, setErr] = useState('');
  const isAdmin = admin.available();
  useEffect(() => { lord.timedate.refresh(); }, []);
  const clock = useClock((d) => `${formatDate(d, 'long')} · ${formatTime(d, { seconds: true })}`, 1000);
  const preview = useClock((d) => menuClock(d), 1000);
  const patch = (x) => set({ clock: { ...c, ...x } });
  const ntp = !!td?.ntp;
  const setNtp = (on) => admin.run('time-ntp', on ? 'on' : 'off').then(() => { setErr(''); lord.timedate.refresh(); }, (e) => setErr(e.message));
  const pickZone = (z) => {
    setPicking(false);
    admin.run('timezone', z).then(() => { lord.time.reload(); setZone(z); setErr(''); lord.timedate.refresh(); }, (e) => setErr(e.message));
  };
  const now = Math.floor(Date.now() / 1000);
  const off = lord.time.zoneOffset ? lord.time.zoneOffset(zone, now) : null;
  return (
    <Stack>
      <GroupBox footer={isAdmin ? '' : tr('prefs.time.needAdmin')}>
        <SettingRow label={tr('prefs.time.auto')} hint={!td ? '' : !td.canNTP ? tr('prefs.time.noNtp') : ntp ? tr(td.synced ? 'prefs.time.synced' : 'prefs.time.syncing') : tr('prefs.time.manual')}
          disabled={!isAdmin || (td && !td.canNTP)}>
          <Toggle small pending on={ntp} onChange={setNtp} />
        </SettingRow>
        <SettingRow label={tr('prefs.time.dateTime')} hint={clock}>
          <Button small label={tr('prefs.time.change')} disabled={ntp || !isAdmin} onClick={() => setEditing(!editing)} />
        </SettingRow>
        {editing && !ntp ? <ManualTime tr={tr} onDone={() => setEditing(false)} /> : null}
      </GroupBox>
      <GroupBox title={tr('prefs.time.zone')} footer={tr('prefs.time.zoneFooter')}>
        <SettingRow label={zone.split('/').pop().replace(/_/g, ' ')} hint={`${zone.replace(/_/g, ' ')}${off ? ` · ${offsetText(off.offset)} (${off.abbr})` : ''}`}>
          <Button small label={tr('prefs.time.change')} disabled={!isAdmin} onClick={() => setPicking(!picking)} />
        </SettingRow>
        {picking ? <ZonePicker zone={zone} onPick={pickZone} tr={tr} /> : null}
      </GroupBox>
      {err ? <Note tone="warn">{err}</Note> : null}
      <GroupBox title={tr('prefs.time.format')}>
        <SettingRow label={tr('prefs.time.hour24')} hint={tr('prefs.time.hour24Hint')}>
          <PopUp value={c.hour24} onChange={(hour24) => patch({ hour24 })}
            options={[['auto', tr('prefs.time.byRegion')], ['24', tr('prefs.time.h24')], ['12', tr('prefs.time.h12')]]} />
        </SettingRow>
      </GroupBox>
      <GroupBox title={tr('prefs.time.menuClock')}>
        <SettingRow label={tr('prefs.time.showDate')}>
          <PopUp value={c.date} onChange={(date) => patch({ date })}
            options={[['short', tr('prefs.time.dateFull')], ['weekday', tr('prefs.time.dateWeekday')], ['none', tr('prefs.time.dateNone')]]} />
        </SettingRow>
        <SettingRow label={tr('prefs.time.seconds')}>
          <Toggle small on={c.seconds} onChange={(seconds) => patch({ seconds })} />
        </SettingRow>
        <SettingRow label={tr('prefs.time.preview')}>
          <View style={{ paddingHorizontal: 10, height: 24, borderRadius: 6, justifyContent: 'center', backgroundColor: t.mode === 'dark' ? 'rgba(0,0,0,0.3)' : 'rgba(0,0,0,0.06)' }}>
            <Text style={{ fontSize: 13, fontWeight: 500, color: t.text }}>{preview}</Text>
          </View>
        </SettingRow>
      </GroupBox>
    </Stack>
  );
}
