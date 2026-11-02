// quicklook.jsx — Quick Preview contents (Space): an image, the first page of a PDF, text or code,
// a font sample, what is inside an archive and, if the media agent offers it (media.poster), a
// video frame or an audio cover. What comes from outside (PDFs, fonts, archives) is opened by the
// confined helper axia-doc.
import * as sdk from '@lordos/sdk';
import { View, Text, Image, useTheme, formatBytes, DocImage, releaseImage, pdfThumb, fontSample, archive, warmDocs, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { Preview, kindLabel } from './views.jsx';
import { EntryIcon } from './glyphs.jsx';
import { kindOf, extOf } from './kinds.js';
import { extractEntry } from './archive.js';

const isFont = (e) => /^(ttf|otf|ttc|otc)$/.test(extOf(e.name));

// measurement: from opening Quick Preview until the content is there (docs/DOCUMENTS.md,
// "Performance"). The start mark is taken on the first draw, not in an effect.
function useTiming(e, ready) {
  const t0 = useRef(null);
  const path = useRef(null);
  if (path.current !== e?.path) { path.current = e?.path; t0.current = Date.now(); }
  const done = useRef(null);
  if (ready && done.current !== e?.path) {
    done.current = e?.path;
    console.log(`quick-look: ${kindOf(e)} ${e.name} in ${Date.now() - t0.current} ms`);
  }
}

// asynchronous request that returns an image from axia-doc (released on change). It is sent while
// drawing, not in an effect: so the helper works while the panel is painted.
function useDocImage(key, make) {
  const [, force] = useState(0);
  const cur = useRef(null);
  if (key && (!cur.current || cur.current.key !== key)) {
    if (cur.current) { cur.current.alive = false; if (cur.current.img) releaseImage(cur.current.img.image); }
    const st = { key, alive: true, img: null, err: null };
    cur.current = st;
    make().then((r) => { if (!r) return; if (st.alive) { st.img = r; force((n) => n + 1); } else releaseImage(r.image); })
      .catch((x) => { if (st.alive) { st.err = x; force((n) => n + 1); } });
  }
  useEffect(() => () => { const c = cur.current; if (c) { c.alive = false; if (c.img) releaseImage(c.img.image); } }, []);
  return [cur.current?.img || null, cur.current?.err || null];
}

function Footer({ e, th, extra }) {
  return (
    <View style={{ alignItems: 'center', gap: 2, paddingTop: 12 }}>
      <Text style={{ fontSize: 13, fontWeight: 600, color: th.text, textAlign: 'center' }}>{e.name}</Text>
      <Text style={{ fontSize: 11, color: th.textDim, textAlign: 'center' }}>{[kindLabel(e), e.dir ? '' : formatBytes(e.size), extra].filter(Boolean).join(' · ')}</Text>
    </View>
  );
}

function PdfLook({ e, path, th }) {
  // requested right away (without waiting to measure the panel) and shown fitted to the space there is
  const [img, err] = useDocImage(path, () => pdfThumb(path, { width: 400, height: 480 }));
  useTiming(e, !!img || !!err);
  return (
    <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', padding: 12 }}>
      {img ? <DocImage image={img.image} style={{ flex: 1, alignSelf: 'stretch', objectFit: 'contain' }} /> : err ? <EntryIcon e={e} size={140} /> : <Text style={{ fontSize: 12, color: th.textDim }}>{tr('docs.loading')}</Text>}
      <Footer e={e} th={th} extra={img ? tr('docs.pages', { count: img.data.pages || 1 }) : err?.code === 'password' ? tr('docs.locked') : ''} />
    </View>
  );
}

function FontLook({ e, path, th }) {
  const fg = th.mode === 'dark' ? '#f2f2f7' : '#1c1c1e';
  const [img, err] = useDocImage(`${path}|${th.mode}`, () => fontSample(path, { width: 560, fg, lines: [
    { text: tr('fonts.pangram'), size: 34 }, { text: 'ABCDEFGHIJKLMNOPQRSTUVWXYZ', size: 22 }, { text: 'abcdefghijklmnopqrstuvwxyz 0123456789', size: 22 }, { text: tr('fonts.pangram'), size: 14 },
  ] }));
  useTiming(e, !!img || !!err);
  return (
    <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', padding: 12 }}>
      {img ? <DocImage image={img.image} style={{ width: img.w, height: img.h }} /> : err ? <Text style={{ fontSize: 12, color: '#ff453a' }}>{tr('fonts.invalid')}</Text> : null}
      <Footer e={e} th={th} />
    </View>
  );
}

function ArchiveLook({ e, th }) {
  const [info, setInfo] = useState(null);
  useEffect(() => { setInfo(null); archive.list(e.path).then(setInfo).catch((x) => setInfo({ error: x.code || x.message })); }, [e.path]);
  useTiming(e, !!info);
  const files = info?.entries?.filter((x) => !x.d) || [];
  const total = files.reduce((a, x) => a + (x.s || 0), 0);
  return (
    <View style={{ flex: 1, padding: 12, gap: 10, alignItems: 'center' }}>
      <EntryIcon e={e} size={84} />
      <Footer e={e} th={th} extra={info?.entries ? tr('docs.archiveSummary', { count: files.length, size: formatBytes(total) }) : ''} />
      {info?.error ? <Text style={{ fontSize: 12, color: '#ff453a' }}>{tr('docs.err.invalid')}</Text> : null}
      <View style={{ alignSelf: 'stretch', flex: 1, borderRadius: 8, border: `1px solid ${th.separator}`, paddingVertical: 8, paddingHorizontal: 12, overflow: 'hidden', direction: 'ltr',
        backgroundColor: th.mode === 'dark' ? 'rgba(255,255,255,0.03)' : 'rgba(0,0,0,0.02)' }}>
        {(info?.entries || []).slice(0, 18).map((x) => (
          <View key={x.p} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, height: 20 }}>
            <Text style={{ flex: 1, fontSize: 11, fontFamily: 'monospace', color: x.bad ? '#ff453a' : th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{`${x.d ? '▸ ' : '  '}${x.p}`}</Text>
            <Text style={{ fontSize: 11, color: th.textDim }}>{x.d ? '' : formatBytes(x.s)}</Text>
          </View>
        ))}
        {info?.entries?.length > 18 ? <Text style={{ fontSize: 11, color: th.textDim }}>{tr('docs.more', { count: info.entries.length - 18 })}</Text> : null}
      </View>
    </View>
  );
}

// a video frame or an audio cover: only if the SDK has media.poster (media agent)
function MediaLook({ e, th }) {
  const poster = sdk.media && typeof sdk.media.poster === 'function' ? sdk.media.poster : null;
  const [px, setPx] = useState(null);
  const [tried, setTried] = useState(!poster);
  useEffect(() => {
    setPx(null);
    if (!poster) return undefined;
    let alive = true;
    setTried(false);
    Promise.resolve(poster(e.path, 560)).then((r) => { if (alive) { setPx(r || null); setTried(true); } }).catch(() => alive && setTried(true));
    return () => { alive = false; };
  }, [e.path]);
  useTiming(e, tried);
  return (
    <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', padding: 12 }}>
      {px && px.data ? <Image pixels={px} style={{ width: Math.min(560, px.width), height: Math.min(560, px.width) * px.height / px.width, objectFit: 'contain' }} />
        : px && px.image ? <DocImage image={px.image} style={{ width: px.w, height: px.h }} />
          : <EntryIcon e={e} size={160} />}
      <Footer e={e} th={th} />
    </View>
  );
}

function Plain({ e, th }) {
  useTiming(e, true);
  void th;
  return <Preview e={e} big />;
}

export function QuickContent({ e }) {
  const th = useTheme();
  const [real, setReal] = useState(null);
  const [err, setErr] = useState(null);
  // a file inside an archive: only that one is extracted, to the cache
  useEffect(() => {
    setReal(null); setErr(null);
    if (!e?.arc || e.dir) return;
    extractEntry(e).then(setReal).catch((x) => setErr(x.message));
  }, [e?.path]);
  if (!e) return null;
  if (e.arc && !e.dir) {
    if (err) return <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}><Text style={{ fontSize: 12, color: '#ff453a' }}>{tr('docs.extractError', { name: e.name, error: err })}</Text></View>;
    if (!real) return <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}><Text style={{ fontSize: 12, color: th.textDim }}>{tr('docs.loading')}</Text></View>;
    return <QuickContent e={{ ...e, arc: null, path: real }} />;
  }
  const k = kindOf(e);
  if (k === 'pdf') return <PdfLook e={e} path={e.path} th={th} />;
  if (isFont(e)) return <FontLook e={e} path={e.path} th={th} />;
  if (k === 'archive' && archive.isArchive(e.name)) return <ArchiveLook e={e} th={th} />;
  if (k === 'audio' || k === 'video') return <MediaLook e={e} th={th} />;
  return <Plain e={e} th={th} />;
}
