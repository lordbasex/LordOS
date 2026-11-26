// Settings → Appearance → Desktop style: the gallery of every installed theme (themes plan, stage 1; docs/THEMES-PLAN.md).
// A search field, the filters All / Official / Community / Installed and a grid of cards that wraps with the window;
// a card opens the theme's page (a larger picture, its description, author, license, version, its own options from
// theme.json and Apply). A theme that couldn't start in this session (lord-server went back to the one before) says so
// on its card, with Report: the GitHub issue form, filled in, in the browser; nothing is sent by itself.
// Changing the style still applies at once, without logging out (sdk/index.js themes.use). The pure parts (search,
// filters, order, arrow keys, the report's address) are sdk/themegallery.js.
import {
  View, Text, useTheme, useLocale, themes, wm, spacing, typeScale, SearchField, Segmented, Button, DesktopStyleThumb,
  direction, openUrl, lordosVersion, useKeyboard, FilePicker, Layer, ConfirmDialog, useAppWindow,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { galleryEntry, filterGallery, filterCounts, gridStep, gridColumns, themeIssueUrl, GALLERY_FILTERS } from '../../sdk/themegallery.js';
import { Section, ValueRow, Note, tint } from './ui.jsx';
import { ThemeOptions } from './ThemeExtras.jsx';

const CARD = 164, GAP = 12;

// The width of a box after layout (the page column narrows with the window), as Displays does.
function useWidth(ref, fallback) {
  const [w, setW] = useState(fallback);
  useEffect(() => {
    const measure = () => {
      const n = ref.current;
      if (!n) return;
      const x = Math.round(lord.getLayout(n.id)[2]);
      if (x > 0) setW((old) => (Math.abs(x - old) > 1 ? x : old));
    };
    measure();
    const id = setTimeout(measure, 60);           // the first layout may not be done yet
    return () => clearTimeout(id);
  });
  return w;
}

function versionText() {
  try { const v = lordosVersion(); return `${v.version}${v.channel ? ` · ${v.channel}` : ''}`; } catch (e) { return ''; }
}

// The themes as gallery entries; read again on each render (cheap: a directory listing of a few themes).
function useGallery() {
  let version = '';
  try { version = lordosVersion().version; } catch (e) { version = ''; }
  return themes.list().map((th) => galleryEntry(th, { lordosVersion: version }));
}

// Switches the style live; Settings comes back on this page in the new one.
function apply(id) {
  wm.open('settings', { args: { section: 'appearance' } });
  themes.use(id);
}

// (0.8.9.6.2) A sheet over this Settings window: the file dialog, a confirmation (docs/THEMES-GUIDE.md "Community themes")
function Sheet({ children }) {
  const win = useAppWindow();
  const g = wm.frame(wm.get(win.id) || {});
  return (
    <Layer name="modal">
      <View style={{ position: 'absolute', left: g.x || 0, top: g.y || 0, width: g.w || 640, height: g.h || 480 }}>{children}</View>
    </Layer>
  );
}

// What an installed community theme may read, in words
function capsText(tr, caps) {
  const list = (caps || []).map((c) => tr(`settings.style.cap.${c}`)).filter(Boolean);
  return list.length ? list.join(', ') : tr('settings.style.capsNone');
}

// The one-time confirmation before a community theme installed from a file first runs: it names the author, says
// what such a theme can and can't do and what it may read. Only Settings asks; the answer is lord-server's to keep
// (lord.themes.trust, which a theme's own code can't call).
function TrustSheet({ e, onDone }) {
  const { t: tr } = useLocale();
  return (
    <Sheet>
      <ConfirmDialog cautious title={tr('settings.style.trustTitle')}
        detail={`${tr('settings.style.trustDetail', { name: e.name, author: e.author || tr('settings.style.unknownAuthor') })} ${tr('settings.style.trustCaps', { list: capsText(tr, e.caps) })}`}
        okLabel={tr('settings.style.trustOk')} onCancel={() => onDone(false)}
        onOk={() => onDone(themes.trust(e.id))} />
    </Sheet>
  );
}

function applyEntry(e, ask) {
  if (e.user && !e.trusted) ask(e);
  else apply(e.id);
}

// The small pill under a name: "Community · unofficial", "In use".
function Pill({ label, color }) {
  return (
    <View style={{ alignSelf: 'flex-start', paddingHorizontal: 6, height: 16, borderRadius: 8, justifyContent: 'center', backgroundColor: `${color}26` }}>
      <Text style={{ fontSize: 10, fontWeight: 600, color, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}

// The picture of a style, `k` times its 132 × 82 size (scale paints only, so the box around it has the scaled size).
function Preview({ e, k = 1 }) {
  const t = useTheme();
  const thumb = <DesktopStyleThumb id={e.id} accent={t.accent} dark={t.mode === 'dark'} sketch={e.sketch} />;
  if (k === 1) return thumb;
  return (
    <View style={{ width: 132 * k, height: 82 * k, direction: 'ltr' }}>
      <View style={{ width: 132, height: 82, transform: `scale(${k})`, transformOrigin: 'left top' }}>{thumb}</View>
    </View>
  );
}

// "Couldn't start — back on ‹the one in use›" and Report.
function Failed({ e, current, small }) {
  const { t: tr } = useLocale();
  const [msg, setMsg] = useState(null);
  const report = (ev) => {
    ev?.stopPropagation?.();
    const ok = openUrl(themeIssueUrl({ theme: e, fallback: current, version: versionText() }));
    setMsg(ok ? null : tr('settings.style.reportNoBrowser'));
  };
  return (
    <View style={{ gap: 4 }}>
      <Text style={{ fontSize: typeScale.caption, color: tint.error, lineHeight: 1.35, textAlign: 'start' }}>
        {tr('settings.style.failed', { name: current ? current.name : '—' })}
      </Text>
      <View style={{ flexDirection: 'row', gap: spacing.s }}>
        <Button small label={tr('settings.style.report')} onClick={report} />
        {e.user && !small ? <Button small label={tr('settings.style.retry')} onClick={() => { themes.retry(e.id); setMsg(null); }} /> : null}
      </View>
      {small ? null : <Note>{tr('settings.style.reportHint')}</Note>}
      {msg ? <Note tone="warn">{msg}</Note> : null}
    </View>
  );
}

function Card({ e, current, lit, onOpen }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [hover, setHover] = useState(false);
  const ring = lit ? (t.focusRing || t.accent) : e.current ? t.accent : 'transparent';
  return (
    <View onClick={onOpen} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: CARD, padding: 8, gap: 6, borderRadius: 10, border: `2px solid ${ring}`,
        backgroundColor: hover || lit ? t.hover : 'transparent' }}>
      <View style={{ alignItems: 'center' }}><Preview e={e} /></View>
      <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{e.name}</Text>
      <Text style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>
        {[e.author || tr('settings.style.unknownAuthor'), e.version ? tr('settings.style.version', { version: e.version }) : ''].filter(Boolean).join(' · ')}
      </Text>
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 4 }}>
        {e.current ? <Pill label={tr('settings.style.inUse')} color={t.accent} /> : null}
        {e.community ? <Pill label={tr('settings.style.community')} color={tint.warn} /> : null}
      </View>
      {e.failed ? <Failed e={e} current={current} small /> : null}
    </View>
  );
}

// The page of one theme.
function Detail({ e, current, onBack }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [asking, setAsking] = useState(null);       // 'trust' or 'remove'
  useKeyboard((ev) => { if (ev.key === 'Escape' && !asking) onBack(); });
  return (
    <View style={{ gap: spacing.l }}>
      <View style={{ flexDirection: 'row' }}><Button small label={tr('settings.style.back')} onClick={onBack} /></View>
      <Section>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: spacing.l, paddingVertical: spacing.m }}>
          <Preview e={e} k={2} />
          <View style={{ flex: 1, minWidth: 200, gap: spacing.s }}>
            <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{e.name}</Text>
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 4 }}>
              {e.current ? <Pill label={tr('settings.style.inUse')} color={t.accent} /> : null}
              {e.community ? <Pill label={tr('settings.style.community')} color={tint.warn} /> : null}
            </View>
            {e.description ? <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{e.description}</Text> : null}
            {e.failed ? <Failed e={e} current={current} /> : null}
            <View style={{ flexDirection: 'row' }}>
              {e.current ? <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('settings.style.inUseLong')}</Text>
                : <Button primary label={tr('settings.style.apply')} disabled={e.failed} onClick={() => applyEntry(e, () => setAsking('trust'))} />}
              {e.user && !e.current ? <Button label={tr('settings.style.remove')} onClick={() => setAsking('remove')} /> : null}
            </View>
          </View>
        </View>
      </Section>
      <Section footer={e.user ? `${tr('settings.style.communityNoteInstalled')} ${tr('settings.style.rescueHint')}` : e.community ? tr('settings.style.communityNote') : tr('settings.style.hint')}>
        <ValueRow label={tr('settings.style.author')} value={e.author || tr('settings.style.unknownAuthor')} />
        {e.user ? <ValueRow label={tr('settings.style.capsLabel')} value={capsText(tr, e.caps)} /> : null}
        <ValueRow label={tr('settings.style.license')} value={e.license || tr('settings.style.notStated')} />
        <ValueRow label={tr('settings.style.versionLabel')} value={e.version || tr('settings.style.notStated')} />
      </Section>
      <ThemeOptions theme={e} title={tr('settings.style.options')} />
      {asking === 'trust' ? <TrustSheet e={e} onDone={(ok) => { setAsking(null); if (ok) apply(e.id); }} /> : null}
      {asking === 'remove' ? (
        <Sheet>
          <ConfirmDialog destructive title={tr('settings.style.removeTitle', { name: e.name })} detail={tr('settings.style.removeDetail')}
            okLabel={tr('settings.style.remove')} onCancel={() => setAsking(null)} onOk={() => { setAsking(null); if (themes.remove(e.id)) onBack(); }} />
        </Sheet>
      ) : null}
    </View>
  );
}

// Install from a file: the file dialog for a .zip, then lord-themec's checks and build (sdk/index.js themes.install)
function InstallFromFile({ onInstalled }) {
  const { t: tr } = useLocale();
  const [picking, setPicking] = useState(false);
  const [state, setState] = useState(null);         // { busy } | { error, why, findings }
  const picked = (paths) => {
    setPicking(false);
    const path = [].concat(paths).filter(Boolean)[0];
    if (!path) return;
    setState({ busy: true });
    themes.install(path).then((r) => {
      if (r && r.ok) { setState(null); onInstalled(r.id); return; }
      const findings = (r?.report?.findings || []).filter((f) => f && f.level === 'error').slice(0, 8);
      setState({ error: r?.error || 'start', why: r?.why || '', findings });
    });
  };
  return (
    <View style={{ gap: spacing.xs }}>
      <View style={{ flexDirection: 'row' }}>
        <Button small label={tr('settings.style.install')} disabled={!!state?.busy} onClick={() => { setState(null); setPicking(true); }} />
      </View>
      {state?.busy ? <Note>{tr('settings.style.installing')}</Note> : null}
      {state?.error ? (
        <View style={{ gap: 2 }}>
          <Note tone="error">{state.error === 'officialId' ? tr('settings.style.installOfficial') : tr('settings.style.installFailed')}</Note>
          {state.findings.length ? <Note>{tr('settings.style.installProblems')}</Note> : null}
          {state.findings.map((f, i) => (
            <Text key={i} style={{ fontSize: typeScale.caption, color: tint.error, textAlign: 'start', direction: 'ltr' }}>
              {`${f.file ? `${f.file}${f.line ? `:${f.line}` : ''}: ` : ''}${String(f.message || '').slice(0, 200)}`}
            </Text>
          ))}
        </View>
      ) : null}
      {picking ? (
        <Sheet>
          <FilePicker title={tr('settings.style.installPick')} filter={/\.zip$/i} onClose={() => setPicking(false)} onPick={picked} />
        </Sheet>
      ) : null}
    </View>
  );
}

export function ThemeGallery() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [query, setQuery] = useState('');
  const [filter, setFilter] = useState('all');
  const [open, setOpen] = useState(null);           // the id of the theme whose page is open
  const [lit, setLit] = useState(-1);               // the card the arrow keys are on (while the grid has the keys)
  const grid = useRef(null);
  const width = useWidth(grid, 3 * CARD + 2 * GAP);
  const all = useGallery();
  if (!all.length) return null;
  const current = all.find((e) => e.current) || null;
  const shown = filterGallery(all, { query, filter });
  const counts = filterCounts(all, query);
  const cols = gridColumns(width, CARD, GAP);

  const openEntry = open ? all.find((e) => e.id === open) : null;
  if (openEntry) return <Detail e={openEntry} current={current} onBack={() => setOpen(null)} />;

  const keys = (ev) => {
    if (ev.key === 'Enter' || ev.key === ' ') {
      const e = shown[lit < 0 ? 0 : lit];
      if (e) setOpen(e.id);
      return true;
    }
    const i = gridStep(lit, shown.length, cols, ev.key, direction() === 'rtl');
    if (i === lit || !['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(ev.key)) return false;
    setLit(i);
    return true;
  };
  const empty = !shown.length
    ? (query.trim() ? tr('settings.style.noMatch', { query: query.trim() }) : counts[filter] === 0 ? tr('settings.style.noneHere') : '')
    : '';
  return (
    <Section title={tr('settings.style.title')} footer={tr('settings.style.hint')}>
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: spacing.s, paddingTop: spacing.m }}>
        <SearchField value={query} onChange={(q) => { setQuery(q); setLit(-1); }} placeholder={tr('settings.style.search')} style={{ flex: 1, minWidth: 180 }} />
        <Segmented small appearance="neutral" value={filter} onChange={(f) => { setFilter(f); setLit(-1); }}
          options={GALLERY_FILTERS.map((f) => [f, tr(`settings.style.filter.${f}`)])} />
      </View>
      {lord.themes.install ? <InstallFromFile onInstalled={(id) => setOpen(id)} /> : null}
      <View ref={grid} focusable={shown.length > 0} focusRing={false} onKeyDown={keys} onFocus={() => setLit((i) => (i < 0 ? 0 : i))} onBlur={() => setLit(-1)}
        style={{ flexDirection: 'row', flexWrap: 'wrap', gap: GAP, paddingVertical: spacing.m, justifyContent: 'flex-start' }}>
        {shown.map((e, i) => <Card key={e.id} e={e} current={current} lit={i === lit} onOpen={() => setOpen(e.id)} />)}
        {empty ? <Text style={{ flex: 1, paddingVertical: spacing.l, fontSize: typeScale.callout, color: t.textDim, textAlign: 'center' }}>{empty}</Text> : null}
      </View>
    </Section>
  );
}
