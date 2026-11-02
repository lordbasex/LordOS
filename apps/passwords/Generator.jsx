// Generator.jsx — the password and passphrase generator (configurable). It generates in
// axia-vault (getrandom; passphrases from the EFF list of 7776 words) and shows the strength.
// Layout: the result in a monospaced box with its strength under it, Another / Copy (and Use),
// a neutral Password / Passphrase switch and the options as an inset group (docs/DESIGN.md).
import { View, Text, useTheme, useStore, Button, Toggle, Segmented, Slider, Stepper, TextInput, t } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { app, req, copySecret, setMeta } from './api.js';
import { StrengthBar, Sheet, Hint, Group } from './ui.jsx';

export const GEN_DEFAULTS = { mode: 'password', length: 20, upper: true, lower: true, digits: true, symbols: true, noAmbiguous: false,
  words: 5, sep: '-', capitalize: true, addDigit: true };

function Opt({ label, children }) {
  const th = useTheme();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, minHeight: 32 }}>
      <Text style={{ flex: 1, minWidth: 0, fontSize: 13, color: th.text }}>{label}</Text>
      {children}
    </View>
  );
}

export function GeneratorPanel({ onUse }) {
  const th = useTheme();
  const meta = useStore(app, (s) => s.meta);
  const [o, setO] = useState({ ...GEN_DEFAULTS, ...(meta?.gen || {}) });
  const [res, setRes] = useState(null);
  const set = (patch) => setO((x) => ({ ...x, ...patch }));
  const gen = async () => {
    const args = o.mode === 'words' ? { words: o.words, sep: o.sep, capitalize: o.capitalize, addDigit: o.addDigit }
      : { length: o.length, upper: o.upper, lower: o.lower, digits: o.digits, symbols: o.symbols, noAmbiguous: o.noAmbiguous };
    try { setRes(await req('generate', args)); } catch (e) { setRes(null); }
  };
  useEffect(() => { gen(); }, [JSON.stringify(o)]);
  const save = () => setMeta({ gen: o }).catch(() => {});
  return (
    <View style={{ gap: 12 }}>
      <Segmented appearance="neutral" fill value={o.mode} onChange={(mode) => set({ mode })} options={[['password', t('passwords.gen.modePassword')], ['words', t('passwords.gen.modeWords')]]} />
      <View style={{ gap: 8 }}>
        <View style={{ paddingHorizontal: 14, paddingVertical: 10, borderRadius: 10, minHeight: 52, justifyContent: 'center', direction: 'ltr',
          border: `1px solid ${th.separator}`, backgroundColor: th.mode === 'dark' ? 'rgba(255,255,255,0.05)' : 'rgba(0,0,0,0.03)' }}>
          <Text style={{ fontSize: 16, fontFamily: 'monospace', color: th.text, lineHeight: 22, textAlign: 'center' }}>{res?.password || ''}</Text>
        </View>
        {res ? <StrengthBar score={res.score} bits={res.bits} /> : <View style={{ height: 21 }} />}
      </View>
      {o.mode === 'password' ? (
        <Group>
          <Opt label={t('passwords.gen.length', { n: o.length })}><Slider value={o.length} min={8} max={64} width={150} onChange={(v) => set({ length: Math.round(v) })} /></Opt>
          <Opt label={t('passwords.gen.upper')}><Toggle small on={o.upper} onChange={(upper) => set({ upper })} /></Opt>
          <Opt label={t('passwords.gen.lower')}><Toggle small on={o.lower} onChange={(lower) => set({ lower })} /></Opt>
          <Opt label={t('passwords.gen.digits')}><Toggle small on={o.digits} onChange={(digits) => set({ digits })} /></Opt>
          <Opt label={t('passwords.gen.symbols')}><Toggle small on={o.symbols} onChange={(symbols) => set({ symbols })} /></Opt>
          <Opt label={t('passwords.gen.noAmbiguous')}><Toggle small on={o.noAmbiguous} onChange={(noAmbiguous) => set({ noAmbiguous })} /></Opt>
        </Group>
      ) : (
        <View style={{ gap: 6 }}>
          <Group>
            <Opt label={t('passwords.gen.words')}><Stepper value={o.words} min={3} max={12} onChange={(words) => set({ words })} /></Opt>
            <Opt label={t('passwords.gen.sep')}><TextInput value={o.sep} onChange={(sep) => set({ sep: sep.slice(0, 3) })} style={{ width: 64, height: 24 }} /></Opt>
            <Opt label={t('passwords.gen.capitalize')}><Toggle small on={o.capitalize} onChange={(capitalize) => set({ capitalize })} /></Opt>
            <Opt label={t('passwords.gen.addDigit')}><Toggle small on={o.addDigit} onChange={(addDigit) => set({ addDigit })} /></Opt>
          </Group>
          <View style={{ paddingHorizontal: 8 }}><Hint>{t('passwords.gen.wordsHint')}</Hint></View>
        </View>
      )}
      <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'flex-end' }}>
        <Button icon="↻" label={t('passwords.gen.again')} onClick={gen} />
        <Button primary={!onUse} label={t('passwords.gen.copy')} onClick={() => res && copySecret(res.password, t('passwords.gen.password'))} />
        {onUse && <Button primary label={t('passwords.gen.use')} onClick={() => { if (res) { save(); onUse(res.password); } }} />}
      </View>
    </View>
  );
}

export function GeneratorSheet({ onClose, onUse }) {
  return (
    <Sheet title={t('passwords.gen.title')} onClose={onClose} width={420}>
      <GeneratorPanel onUse={onUse ? (p) => { onUse(p); onClose(); } : null} />
    </Sheet>
  );
}
