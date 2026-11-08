// Settings → AI: reading aloud (docs/VOICE.md). Voice: on or off, the ❖Esc shortcut, the small player (settings.voice,
// sdk/voice.js) and where speech is made (~/.config/lordos/voice.json's backend, which `say` reads too).
import { Toggle, Segmented, useLocale, useSettings, fs, VOICE_DEFAULTS, speakAvailable } from '@lordos/sdk';
import { useState } from 'react';
import { Stack, Section, SettingRow, Note } from './ui.jsx';

const VOICE_JSON = '.config/lordos/voice.json';
const BACKENDS = ['auto', 'cpu', 'gpu'];

function readVoiceJson() {
  try { const j = JSON.parse(fs.read(VOICE_JSON)); return j && typeof j === 'object' && !Array.isArray(j) ? j : {}; } catch (e) { return {}; }
}

export function AISettings() {
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const v = { ...VOICE_DEFAULTS, ...(s.voice || {}) };
  const put = (patch) => set({ voice: { ...(s.voice || {}), ...patch } });
  const [backend, setBackend] = useState(() => (BACKENDS.includes(readVoiceJson().backend) ? readVoiceJson().backend : 'auto'));
  const [error, setError] = useState('');
  const pickBackend = (b) => {
    try {
      try { fs.mkdir('.config/lordos'); } catch (e) { /* already there */ }
      fs.write(VOICE_JSON, `${JSON.stringify({ ...readVoiceJson(), backend: b }, null, 2)}\n`);
      setBackend(b);
      setError('');
    } catch (e) { setError(e.message); }
  };
  const installed = speakAvailable();
  return (
    <Stack>
      <Section title={tr('settings.ai.voice')} footer={tr('settings.ai.voiceFooter')}>
        <SettingRow label={tr('settings.ai.readAloud')} hint={tr('settings.ai.readAloudHint')}>
          <Toggle small on={v.enabled} onChange={(enabled) => put({ enabled })} />
        </SettingRow>
        <SettingRow label={tr('settings.ai.shortcut')} hint={tr('settings.ai.shortcutHint')} disabled={!v.enabled}>
          <Toggle small on={v.shortcut} onChange={(shortcut) => v.enabled && put({ shortcut })} />
        </SettingRow>
        <SettingRow label={tr('settings.ai.player')} hint={tr('settings.ai.playerHint')} disabled={!v.enabled}>
          <Toggle small on={v.player} onChange={(player) => v.enabled && put({ player })} />
        </SettingRow>
        <SettingRow label={tr('settings.ai.backend')} hint={tr('settings.ai.backendHint')}>
          <Segmented small value={backend} onChange={pickBackend} options={BACKENDS.map((b) => [b, tr(`settings.ai.backend.${b}`)])} />
        </SettingRow>
      </Section>
      <Section title={tr('settings.ai.model')}>
        <SettingRow label={tr(installed ? 'settings.ai.installed' : 'settings.ai.notInstalled')} hint={tr('settings.ai.modelHint')} />
      </Section>
      {error ? <Note tone="error">{error}</Note> : null}
      <Note>{tr('settings.ai.privacy')}</Note>
    </Stack>
  );
}
