// Help — help articles for every app and feature, with search, and the first-login "Welcome to
// LordOS" tour (docs/SYSTEM.md). Articles are markdown in assets/help/<lang>/ (English is the
// default) and are read by the SDK (sdk/help.js → lord.help). The links settings:<section>,
// app:<id> and help:<article> open Settings, an app or another article.
import {
  View, Text, useTheme, useAppWindow, useLocale, t as tr, help, Markdown, ScrollView, SidebarItem, SectionTitle, Button,
  getApp, wm, appName, settings, textDirection, SearchField, EmptyState, Hairline, AxiaLogo, spacing, typeScale,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';

// Width of a box after layout (re-measured on every render).
function useWidth(ref, initial) {
  const [w, setW] = useState(initial);
  useEffect(() => {
    const id = setTimeout(() => {
      if (!ref.current) return;
      const lw = Math.round(lord.getLayout(ref.current.id)[2]);
      if (lw > 0 && Math.abs(lw - w) > 2) setW(lw);
    }, 20);
    return () => clearTimeout(id);
  });
  return w;
}

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #5ac8fa, #0a60d0)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: s * 0.62, height: s * 0.62, borderRadius: s * 0.31, backgroundColor: 'rgba(255,255,255,0.95)', alignItems: 'center', justifyContent: 'center' }}>
        <Text style={{ fontSize: s * 0.44, fontWeight: 700, color: '#0a60d0', lineHeight: s * 0.62 }}>?</Text>
      </View>
    </View>
  );
}

const SECTIONS = ['start', 'desktop', 'apps', 'system', 'settings'];

// ---- welcome tour ----

function TourArt({ step }) {
  const t = useTheme();
  const icons = [[], ['files', 'terminal', 'store'], [], ['settings', 'monitor'], ['clock', 'help'], ['help']][step] || [];
  const glyph = ['', '', '⌕', '', '', '?'][step];
  return (
    <View style={{ height: 120, flexDirection: 'row', gap: spacing.l, alignItems: 'center', justifyContent: 'center' }}>
      {step === 0 ? <AxiaLogo size={112} /> : null}
      {icons.map((id) => { const a = getApp(id); return a?.Icon ? <a.Icon key={id} size={84} /> : null; })}
      {!icons.length && glyph ? <Text style={{ fontSize: 64, color: t.textDim }}>{glyph}</Text> : null}
    </View>
  );
}

function Tour({ onDone }) {
  const t = useTheme();
  const [step, setStep] = useState(0);
  const steps = 6;
  const last = step === steps - 1;
  const actions = [
    null,
    null,
    null,
    [['settings', 'appearance'], ['settings', 'network'], ['settings', 'users']],
    null,
    null,
  ][step];
  return (
    <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', padding: spacing.xxxl, gap: spacing.l }}>
      <TourArt step={step} />
      <Text style={{ fontSize: 26, fontWeight: 600, color: t.text, textAlign: 'center' }}>{tr(`help.tour.${step}.title`)}</Text>
      <Text style={{ fontSize: 14, color: t.textDim, textAlign: 'center', lineHeight: 1.5, maxWidth: 520 }}>{tr(`help.tour.${step}.body`)}</Text>
      {actions && (
        <View style={{ flexDirection: 'row', gap: spacing.s }}>
          {actions.map(([app, section]) => (
            <Button key={section} label={tr(`settings.section.${section}`)} onClick={() => wm.open(app, { args: { section } })} />
          ))}
        </View>
      )}
      <View style={{ flexDirection: 'row', gap: 6, marginTop: spacing.xs }}>
        {Array.from({ length: steps }, (_, i) => (
          <View key={i} onClick={() => setStep(i)} style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: i === step ? t.accent : t.control }} />
        ))}
      </View>
      <View style={{ flexDirection: 'row', gap: spacing.s, alignItems: 'center', marginTop: spacing.s }}>
        {!last && <Button plain label={tr('help.tour.skip')} onClick={onDone} />}
        {step > 0 && <Button label={tr('help.tour.back')} onClick={() => setStep(step - 1)} />}
        <Button primary label={last ? tr('help.tour.finish') : tr('help.tour.next')} onClick={() => (last ? onDone() : setStep(step + 1))} />
      </View>
    </View>
  );
}

// ---- articles ----

function Article({ id, onOpen }) {
  const t = useTheme();
  const box = useRef(null);
  const width = useWidth(box, 600);
  const a = id ? help.read(id) : null;
  if (!a) return <EmptyState title={tr('help.notFound')} />;
  const app = a.app ? getApp(a.app) : null;
  const onLink = (url) => {
    const m = /^help:([a-z0-9-]+)$/.exec(String(url));
    if (m) { onOpen(m[1]); return; }
    help.link(url);
  };
  // a centered reading column; the article keeps its own direction (an English article in an
  // Arabic interface still reads left to right)
  const col = Math.max(280, Math.min(680, width - 2 * spacing.xxxl));
  return (
    <View ref={box} style={{ flex: 1 }}>
      <ScrollView style={{ flex: 1 }}>
        <View style={{ alignItems: 'center', paddingTop: spacing.xxl, paddingBottom: spacing.xxxl }}>
          <View style={{ width: col, direction: textDirection(a.body) }}>
            {app && (
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m, marginBottom: spacing.s }}>
                <app.Icon size={48} />
                <View style={{ flex: 1 }} />
                <Button label={tr('help.openApp', { app: appName(app) })} onClick={() => wm.open(app.id)} />
              </View>
            )}
            <Markdown source={a.body} width={col} fitWidth onLink={onLink} />
          </View>
        </View>
      </ScrollView>
    </View>
  );
}

let lastArticle = 'welcome';

export default function Help() {
  const t = useTheme();
  const { lang } = useLocale();
  const { args } = useAppWindow();
  const [article, setArticleState] = useState(lastArticle);
  const [tour, setTour] = useState(false);
  const [query, setQuery] = useState('');
  const setArticle = (id) => { lastArticle = id; setArticleState(id); setTour(false); };
  useEffect(() => {
    if (args?.tour) setTour(true);
    else if (args?.article) setArticle(args.article);
  }, [args?.nonce]);
  const list = useMemo(() => help.list(), [lang]);
  const results = useMemo(() => (query.trim().length >= 2 ? help.search(query) : null), [query, lang]);
  const finishTour = () => { setTour(false); settings.set({ welcomeShown: true }); setArticle('welcome'); };
  if (!help.available() || !list.length) return <EmptyState title={tr('help.unavailable')} />;
  return (
    <View style={{ flex: 1, flexDirection: 'row' }}>
      <View style={{ width: 240, backgroundColor: t.sidebarBg }}>
        <View style={{ paddingHorizontal: 10, paddingTop: spacing.m, paddingBottom: spacing.s }}>
          <SearchField value={query} onChange={setQuery} placeholder={tr('help.search')} onSubmit={() => results?.[0] && setArticle(results[0].id)} />
        </View>
        <ScrollView style={{ flex: 1 }}>
          <View style={{ paddingHorizontal: 10, paddingBottom: spacing.m, gap: 2 }}>
            {results ? (
              results.length ? results.map((r) => {
                const on = r.id === article && !tour;
                return (
                  <View key={r.id} onClick={() => setArticle(r.id)} style={{ paddingVertical: spacing.s, paddingHorizontal: 10, borderRadius: 6, gap: 2, backgroundColor: on ? t.accent : 'transparent' }}>
                    <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: on ? '#fff' : t.text, textAlign: 'start' }}>{r.title}</Text>
                    <Text style={{ fontSize: typeScale.caption, color: on ? 'rgba(255,255,255,0.85)' : t.textDim, lineHeight: 1.35, maxHeight: 44, overflow: 'hidden', textAlign: 'start' }}>{r.snippet}</Text>
                  </View>
                );
              }) : <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'center', paddingTop: spacing.xl }}>{tr('help.noResults')}</Text>
            ) : (
              <>
                <View style={{ height: spacing.xs }} />
                <SidebarItem label={tr('help.tour.open')} selected={tour} onClick={() => setTour(true)} icon="☺" />
                {SECTIONS.map((sec) => {
                  const items = list.filter((x) => (x.section || 'apps') === sec);
                  if (!items.length) return null;
                  return (
                    <View key={sec} style={{ gap: 2 }}>
                      <SectionTitle>{tr(`help.section.${sec}`)}</SectionTitle>
                      {items.map((x) => {
                        const app = x.app ? getApp(x.app) : null;
                        return <SidebarItem key={x.id} label={x.title} selected={!tour && x.id === article} onClick={() => setArticle(x.id)}
                          icon={app?.Icon ? <View style={{ width: 18, alignItems: 'center' }}><app.Icon size={18} /></View> : '≡'} />;
                      })}
                    </View>
                  );
                })}
              </>
            )}
          </View>
        </ScrollView>
        <Hairline edge="end" />
      </View>
      <View style={{ flex: 1, minWidth: 0 }}>
        {tour ? <Tour onDone={finishTour} /> : <Article id={article} onOpen={setArticle} />}
      </View>
    </View>
  );
}
