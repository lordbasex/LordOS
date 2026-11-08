// livespeech.jsx — Live Speech (docs/VOICE.md): type something and LordOS says it aloud, with the voice chosen for the
// text's language in Settings → AI → Voice (a personal voice too). Meta+Shift+Escape opens and closes it
// (sdk/shortcuts.js) while Settings → AI's "Live Speech" is on; sdk/index.js mounts it in the overlay layer.
//
//   liveSpeech.toggle() / liveSpeech.close()
//
// Enter says the text and keeps the panel open for the next; the last phrases stay below it to be said again with a
// click (only in memory: they go when LordOS restarts). Esc closes it. The player shows as for any reading.
import { useState } from 'react';
import { View, Text, mainBox } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { useTheme, ThemeContext, DEFAULT_TOKENS, shellTokens } from './theme.js';
import { t } from './i18n.js';
import { TextInput } from './components.jsx';
import { speak, speaking, stopSpeaking, voiceSettings } from './voice.js';

const open = createStore(false);
const recent = createStore([]);
const RECENT = 6;

export const liveSpeech = {
  toggle: () => { if (open.get()) open.set(false); else if (voiceSettings().enabled && voiceSettings().liveSpeech) open.set(true); },
  close: () => open.set(false),
};

function say(text) {
  const s = String(text || '').trim();
  if (!s) return;
  if (speak(s, null, t('shell.liveSpeech.title'))) recent.set((r) => [s, ...r.filter((x) => x !== s)].slice(0, RECENT));
}

function Panel() {
  const th = useTheme();
  const [text, setText] = useState('');
  const list = useStore(recent);
  const on = useStore(speaking);
  const box = mainBox();
  const W = Math.min(640, box.width - 40);
  return (
    <View onClick={() => open.set(false)} style={{ position: 'absolute', ...box }}>
      <View onClick={(e) => e.stopPropagation()} style={{ position: 'absolute', left: Math.round((box.width - W) / 2), top: Math.round(box.height * 0.16),
        width: W, borderRadius: 14, backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, boxShadow: '0 24px 60px rgba(0,0,0,0.5)', overflow: 'hidden' }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 14, paddingVertical: 10 }}>
          <Text style={{ fontSize: 18, color: on ? th.accent : th.textDim }}>❝</Text>
          <TextInput value={text} onChange={setText} autoFocus size={18} placeholder={t('shell.liveSpeech.placeholder')}
            onSubmit={() => { say(text); setText(''); }} onCancel={() => { if (on) stopSpeaking(); else open.set(false); }}
            style={{ flex: 1, backgroundColor: 'transparent', border: '0 solid transparent' }} />
        </View>
        {list.length ? <View style={{ height: 1, backgroundColor: th.separator }} /> : null}
        {list.length ? (
          <View style={{ paddingVertical: 6 }}>
            <Text style={{ fontSize: 10, fontWeight: 700, color: th.textDim, paddingHorizontal: 14, paddingBottom: 2 }}>{t('shell.liveSpeech.recent').toUpperCase()}</Text>
            {list.map((s) => (
              <View key={s} onClick={() => say(s)} style={{ marginHorizontal: 6, paddingHorizontal: 8, paddingVertical: 7, borderRadius: 7 }}>
                <Text style={{ fontSize: 13, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{s}</Text>
              </View>
            ))}
          </View>
        ) : null}
        <Text style={{ fontSize: 11, color: th.textDim, paddingHorizontal: 14, paddingBottom: 10 }}>{t('shell.liveSpeech.hint')}</Text>
      </View>
    </View>
  );
}

export function LiveSpeechPanel() {
  const isOpen = useStore(open);
  const tokens = useStore(shellTokens);
  if (!isOpen) return null;
  return <ThemeContext.Provider value={tokens || DEFAULT_TOKENS}><Panel /></ThemeContext.Provider>;
}
