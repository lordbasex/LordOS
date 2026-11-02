// Blocklists.jsx — Settings → Privacy → "Blocklists" (0.8.5; registered with registerPrivacySection,
// like the other Monitor items). See docs/FIREWALL.md, "Blocklists".
//   - the curated catalog by topic (advertising, trackers, malware), each list with its source, its
//     licence, how many entries it has and when it was last downloaded; a checkbox turns it on;
//   - custom lists: an https address and its format (hosts file, one domain per line, one address
//     or network per line);
//   - "Update now"; otherwise lord-server downloads the enabled lists once a day.
// Lists never override a rule the owner wrote for a destination, and lord-server writes every file
// (lord.fw.*List); this screen writes none.
import { View, Text, Button, Segmented, GroupBox, TextInput, useTheme, useLocale, useStore, firewall, firewallState, spacing, typeScale, formatNumber, formatDate, formatTime, tOptional, errorText } from '@lordos/sdk';
import { useEffect, useState } from 'react';

const TOPICS = ['ads', 'trackers', 'malware', 'custom'];

function CheckBox({ on, onChange }) {
  const t = useTheme();
  return (
    <View onClick={() => onChange(!on)} style={{ width: 16, height: 16, borderRadius: 4, border: `1px solid ${on ? t.accent : t.textDim}`,
      backgroundColor: on ? t.accent : 'transparent', alignItems: 'center', justifyContent: 'center' }}>
      {on ? <Text style={{ fontSize: 11, color: '#fff', fontWeight: 700 }}>✓</Text> : null}
    </View>
  );
}

const when = (s) => { const d = new Date(s * 1000); return `${formatDate(d)} ${formatTime(d)}`; };

function ListRow({ l, tr, onToggle, onRemove }) {
  const t = useTheme();
  const name = l.custom ? l.url.replace(/^https:\/\//, '') : tOptional(`firewall.lists.name.${l.id}`) || l.id;
  const entries = l.domains + l.nets;
  const status = l.error ? tr('firewall.lists.failed', { error: tOptional(`firewall.lists.err.${l.error.split(' ')[0]}`) || l.error })
    : l.updated ? tr('firewall.lists.updated', { count: formatNumber(entries, 0), when: when(l.updated) })
      : l.on ? tr('firewall.lists.pending') : l.approx ? tr('firewall.lists.about', { count: formatNumber(l.approx, 0) }) : '';
  return (
    <View style={{ minHeight: 44, flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: 7 }}>
      <CheckBox on={l.on} onChange={onToggle} />
      <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
        <Text style={{ fontSize: typeScale.body, color: t.text, textAlign: 'start' }} numberOfLines={2}>{name}</Text>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35, textAlign: 'start' }} numberOfLines={2}>
          {l.custom ? tr(`firewall.lists.format.${l.format}`) : tr('firewall.lists.source', { source: l.source, license: l.license })}
        </Text>
        {status ? <Text style={{ fontSize: typeScale.caption, color: l.error ? '#ff9f0a' : t.textDim, lineHeight: 1.35, textAlign: 'start' }}>
          {status}{l.truncated ? ` · ${tr('firewall.lists.truncated')}` : ''}</Text> : null}
      </View>
      {onRemove ? <Button small label={tr('firewall.remove')} onClick={onRemove} /> : null}
    </View>
  );
}

export function BlocklistsPrivacy() {
  const t = useTheme();
  const { t: tr } = useLocale();
  useStore(firewallState);
  const [st, setSt] = useState(() => firewall.lists());
  const [url, setUrl] = useState('');
  const [format, setFormat] = useState('domains');
  const [msg, setMsg] = useState(null);
  // while a job runs (a download, a compile) the state is read again every 2 s
  useEffect(() => {
    const id = setInterval(() => setSt(firewall.lists()), st?.busy ? 1500 : 6000);
    return () => clearInterval(id);
  }, [st?.busy]);
  if (!firewall.available() || !st) return null;
  const refresh = () => setSt(firewall.lists());
  const toggle = (l, on) => { try { firewall.setList(l.id, on); } catch (e) { setMsg(errorText(e)); } refresh(); };
  const add = () => {
    try { firewall.addList(url.trim(), format); setUrl(''); setMsg(null); } catch (e) { setMsg(e.message || errorText(e)); }
    refresh();
  };
  const head = (text) => <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, paddingHorizontal: spacing.xs, textAlign: 'start' }}>{text}</Text>;
  const foot = (text, color) => <Text style={{ fontSize: typeScale.caption, color: color || t.textDim, lineHeight: 1.4, marginHorizontal: spacing.xs, textAlign: 'start' }}>{text}</Text>;
  const on = st.lists.filter((l) => l.on).length;
  return (
    <View style={{ gap: 20 }}>
      <View style={{ gap: 6 }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', minHeight: 22, paddingHorizontal: spacing.xs, gap: spacing.s }}>
          <Text style={{ flex: 1, fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('firewall.lists.title')}</Text>
          <Button small plain label={st.busy ? tr('firewall.lists.working') : tr('firewall.lists.update')} disabled={st.busy || !on}
            onClick={() => { firewall.updateLists(true); refresh(); }} />
        </View>
        {foot(tr('firewall.lists.summary', { lists: formatNumber(on, 0), entries: formatNumber(st.domains + st.nets, 0) }))}
      </View>
      {TOPICS.map((topic) => {
        const lists = st.lists.filter((l) => l.topic === topic);
        if (!lists.length && topic !== 'custom') return null;
        return (
          <View key={topic} style={{ gap: 6 }}>
            {head(tr(`firewall.lists.topic.${topic}`))}
            <GroupBox>
              {lists.map((l) => <ListRow key={l.id} l={l} tr={tr} onToggle={(v) => toggle(l, v)}
                onRemove={l.custom ? () => { try { firewall.removeList(l.id); } catch (e) { setMsg(e.message); } refresh(); } : null} />)}
              {topic === 'custom' ? (
                <View style={{ gap: spacing.s, paddingVertical: spacing.s }}>
                  <TextInput value={url} onChange={setUrl} placeholder={tr('firewall.lists.urlPlaceholder')} onSubmit={add} />
                  <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
                    <Segmented small value={format} onChange={setFormat}
                      options={[['hosts', tr('firewall.lists.format.hosts')], ['domains', tr('firewall.lists.format.domains')], ['ips', tr('firewall.lists.format.ips')]]} />
                    <View style={{ flex: 1 }} />
                    <Button small label={tr('firewall.lists.add')} disabled={!/^https:\/\/\S+$/.test(url.trim())} onClick={add} />
                  </View>
                  {msg ? <Text style={{ fontSize: typeScale.caption, color: '#ff453a', textAlign: 'start' }}>{msg}</Text> : null}
                </View>
              ) : null}
            </GroupBox>
            {topic === 'custom' ? foot(tr('firewall.lists.customHint')) : null}
          </View>
        );
      })}
      {foot(tr('firewall.lists.footer'))}
    </View>
  );
}
