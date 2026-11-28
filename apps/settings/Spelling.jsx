// Spelling — Settings → Keyboard's spelling sections (docs/SPELLING.md): check spelling as you type, the dictionaries
// to check with ("Spelling: languages"; none chosen: the interface language's and English, the rule lord-web shares),
// Debian's dictionaries to install (pkg-install, an administrator, only the names of SPELL_PACKAGES), and the
// person's dictionary (the words added with "Add to dictionary", shared with the web apps).
import { View, Text, useTheme, useSettings, useLocale, useStore, Toggle, Button, Checkbox, admin, errorText, formatBytes, tOptional, spacing, typeScale,
  spellVersion, spellInstalled, spellDefaults, spellWordsList, spellRemoveWord, SPELL_PACKAGES, spellPackageOk, packageInstalled, toggleLang, spellLanguages } from '@lordos/sdk';
import { useState } from 'react';
import { Section, SettingRow, Note, Actions } from './ui.jsx';

const progressOf = (line) => { const m = /^progress download (\d+) (\d+)/.exec(String(line || '')); return m ? { done: Number(m[1]), total: Number(m[2]) } : null; };

export function tagName(tag) {
  const [l, cc] = String(tag).split('_');
  const lang = tOptional(`languages.${l}`) || l;
  const country = cc ? tOptional(`countries.${cc}`) || cc : '';
  return country ? `${lang} (${country})` : lang;
}

export function SpellingSettings() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  useStore(spellVersion);
  const [job, setJob] = useState(null);               // { pkg, done, total }
  const [msg, setMsg] = useState(null);
  const [rev, setRev] = useState(0);
  const sp = s.spelling || {};
  const on = sp.on !== false;
  const installed = spellInstalled();
  const defaults = spellDefaults();
  const inUse = spellLanguages(sp.langs, installed, defaults);
  const chosen = Array.isArray(sp.langs) && sp.langs.length > 0;
  const setSp = (patch) => set({ spelling: { ...sp, ...patch } });
  const words = spellWordsList();
  const install = async (pkg) => {
    if (job || !spellPackageOk(pkg)) return;
    setMsg(null);
    setJob({ pkg, done: 0, total: 0 });
    try { await admin.run('pkg-install', pkg, (l) => { const p = progressOf(l); if (p) setJob({ pkg, ...p }); }); } catch (e) { setMsg(e.message || errorText(e)); }
    setJob(null);
    setRev((n) => n + 1);
  };
  const more = SPELL_PACKAGES.filter((p) => !packageInstalled(p, installed));
  return (
    <>
      <Section title={tr('spell.settings.title')} footer={tr('spell.settings.hint')}>
        <SettingRow label={tr('spell.settings.check')}>
          <Toggle small on={on} onChange={(v) => setSp({ on: v })} />
        </SettingRow>
      </Section>
      <Section title={tr('spell.settings.languages')} footer={chosen ? tr('spell.settings.chosenHint') : tr('spell.settings.defaultHint')}>
        {installed.length ? installed.map((tag) => (
          <SettingRow key={tag} label={tagName(tag)} disabled={!on}>
            <Checkbox checked={inUse.includes(tag)} disabled={!on || (!inUse.includes(tag) && inUse.length >= 4)}
              onChange={(v) => setSp({ langs: toggleLang(inUse, tag, v) })} />
          </SettingRow>
        )) : <Text style={{ paddingVertical: spacing.s, fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('spell.settings.none')}</Text>}
        {chosen ? <Actions style={{ paddingVertical: spacing.s }}><Button small label={tr('spell.settings.useDefault')} onClick={() => setSp({ langs: [] })} /></Actions> : null}
      </Section>
      {more.length ? (
        <Section title={tr('spell.settings.more')} footer={tr('spell.settings.moreHint')}>
          {more.map((p) => (
            <SettingRow key={p.pkg} label={tagName(p.match)} hint={p.bytes ? formatBytes(p.bytes) : null}>
              {job && job.pkg === p.pkg
                ? <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{job.total ? `${Math.floor((job.done * 100) / job.total)} %` : tr('spell.settings.installing')}</Text>
                : <Button small label={tr('spell.settings.install')} disabled={!!job} onClick={() => install(p.pkg)} />}
            </SettingRow>
          ))}
          {msg ? <Note tone="error">{msg}</Note> : null}
        </Section>
      ) : null}
      <Section title={tr('spell.settings.words')} footer={tr('spell.settings.wordsHint')}>
        {words.length ? words.map((w) => (
          <SettingRow key={`${w}${rev}`} label={w}>
            <Button small label={tr('spell.settings.remove')} onClick={() => { spellRemoveWord(w); setRev((n) => n + 1); }} />
          </SettingRow>
        )) : <Text style={{ paddingVertical: spacing.s, fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('spell.settings.noWords')}</Text>}
      </Section>
    </>
  );
}
