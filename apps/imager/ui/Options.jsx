// App options: sound and eject when finished, updates of the app, language and theme (the desktop
// app; on LordOS they follow the system), the download cache. No telemetry, ever.
import { View, Text, Button, useTheme, useLocale, typeScale, spacing } from '@lordos/sdk';
import { useState } from 'react';
import { platform } from '../platform.js';
import { set, setOptions } from '../state.js';
import { LANGUAGES } from '../core/catalog.js';
import { Group, SwitchRow, Choice, KeyValue } from './kit.jsx';
import { formatSize } from './format.js';

export function OptionsDialog({ s }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const o = s.options;
  const [cache, setCache] = useState(() => platform.cacheSize());
  const [clearing, setClearing] = useState(false);
  const close = () => set({ dialog: null });
  return (
    <View onClick={close} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.3)', alignItems: 'center', justifyContent: 'center' }}>
      <View onClick={(e) => e.stopPropagation?.()} {...platform.a11y({ role: 'dialog', label: tr('imager.options.title') })}
        style={{ width: 480, maxWidth: '94%', padding: 20, gap: spacing.l, borderRadius: t.groupRadius + 2, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 14px 36px rgba(0,0,0,0.45)' }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text }}>{tr('imager.options.title')}</Text>
        <Group>
          <SwitchRow id="o-sound" label={tr('imager.options.sound')} on={o.sound} onChange={(sound) => setOptions({ sound })} />
          <SwitchRow id="o-eject" label={tr('imager.options.eject')} detail={tr('imager.options.ejectHint')} on={o.eject} onChange={(eject) => setOptions({ eject })} />
          {platform.appUpdates ? (
            <SwitchRow id="o-upd" label={tr('imager.options.updates')}
              detail={o.appUpdates && platform.appUpdate ? tr('imager.options.updateFound', { v: platform.appUpdate.version }) : tr('imager.options.updatesHint')}
              on={o.appUpdates} onChange={(appUpdates) => setOptions({ appUpdates })} />
          ) : (
            <View style={{ paddingVertical: 9 }}><Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.4 }}>{tr('imager.options.updatesSystem')}</Text></View>
          )}
        </Group>
        {platform.followsSystem ? (
          <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.4 }}>{tr('imager.options.followsSystem')}</Text>
        ) : (
          <Group>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: 8 }}>
              <Text style={{ flex: 1, fontSize: typeScale.body, color: t.text }}>{tr('imager.options.language')}</Text>
              <Choice id="o-lang" small label={tr('imager.options.language')} value={o.lang || ''} onChange={(lang) => { setOptions({ lang }); platform.setLanguage?.(lang); }}
                options={[['', tr('imager.options.auto')], ...LANGUAGES.map((l) => [l.id, l.name])]} />
            </View>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: 8 }}>
              <Text style={{ flex: 1, fontSize: typeScale.body, color: t.text }}>{tr('imager.options.theme')}</Text>
              <Choice id="o-theme" small label={tr('imager.options.theme')} value={o.theme} onChange={(theme) => { setOptions({ theme }); platform.setTheme?.(theme); }}
                options={[['auto', tr('imager.options.auto')], ['light', tr('imager.look.light')], ['dark', tr('imager.look.dark')]]} />
            </View>
          </Group>
        )}
        <Group title={tr('imager.options.cache')}>
          <KeyValue label={tr('imager.options.cacheWhere')} value={platform.cacheDir()} mono />
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: 8 }}>
            <Text style={{ flex: 1, fontSize: typeScale.body, color: t.text }}>{tr('imager.options.cacheSize', { size: formatSize(cache) })}</Text>
            {platform.chooseCacheDir ? <Button small label={tr('imager.options.cacheMove')} onClick={async () => { await platform.chooseCacheDir(); setCache(platform.cacheSize()); }} /> : null}
            <Button small label={tr(clearing ? 'imager.options.clearing' : 'imager.options.clear')} disabled={clearing || !cache}
              onClick={async () => { setClearing(true); try { await platform.clearCache(); } finally { setClearing(false); setCache(platform.cacheSize()); } }} />
          </View>
        </Group>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.45 }}>{tr('imager.options.privacy')}</Text>
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end' }}><Button primary label={tr('imager.options.done')} onClick={close} /></View>
      </View>
    </View>
  );
}
