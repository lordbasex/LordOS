// Fonts — font viewer and manager: installed fonts by family, a sample with text and
// sizes of your choice, a character table by block and information; install a .ttf/.otf for the
// user (~/.local/share/fonts) and remove it. Fonts are opened in the confined axia-doc
// helper (FreeType), so a broken or malicious font does not affect the desktop.
import {
  View, Text, TextInput, Slider, Button, Segmented, useTheme, useAppWindow, useKeyboard, wm, fs, clipboard, DocImage, releaseImage,
  fontInfo, fontSample, fontGrid, formatBytes, t as tr, SidebarItem, SectionTitle, SearchField, GroupBox, FormRow, EmptyState,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { BLOCKS } from './blocks.js';

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(160deg, #f5f5f7, #c7c7cc)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, overflow: 'hidden', alignItems: 'center', justifyContent: 'center' }}>
      <Text style={{ position: 'absolute', left: s * 0.12, top: s * 0.06, fontSize: s * 0.62, fontWeight: 700, color: '#1c1c1e' }}>A</Text>
      <Text style={{ position: 'absolute', left: s * 0.52, top: s * 0.36, fontSize: s * 0.44, fontWeight: 400, color: '#d70015' }}>a</Text>
      <View style={{ position: 'absolute', left: s * 0.12, right: s * 0.12, bottom: s * 0.16, height: Math.max(1, s * 0.03), backgroundColor: '#8e8e93' }} />
    </View>
  );
}

const userDir = () => `${fs.home()}/.local/share/fonts`;
const isUser = (p) => !!p && (p.startsWith(`${userDir()}/`) || p.startsWith(`${fs.home()}/.fonts/`));
const FONT_EXT = /\.(ttf|otf|ttc|otc)$/i;

function loadFonts(rescan) {
  let list = [];
  try { list = lord.fonts.list({ rescan }); } catch (e) { list = []; }
  const fams = new Map();
  for (const f of list) {
    if (!f.path) continue;
    if (!fams.has(f.family)) fams.set(f.family, { family: f.family, styles: [], mono: false, color: false, user: false });
    const g = fams.get(f.family);
    if (!g.styles.some((s) => s.path === f.path && s.index === f.index)) g.styles.push(f);
    g.mono ||= f.mono; g.color ||= f.color; g.user ||= isUser(f.path);
  }
  for (const g of fams.values()) g.styles.sort((a, b) => (a.italic - b.italic) || (a.weight - b.weight));
  return [...fams.values()].sort((a, b) => a.family.localeCompare(b.family));
}

// A sample drawn by the helper
function Sample({ font, text, size, width, th, extra }) {
  const [img, setImg] = useState(null);
  const [err, setErr] = useState(false);
  useEffect(() => {
    if (!font || width < 40) return undefined;
    let alive = true, got = null;
    const id = setTimeout(() => {
      const lines = extra ? extra.map((s) => ({ text, size: s })) : [{ text, size }];
      fontSample(font.path, { lines, width, fg: th.mode === 'dark' ? '#f2f2f7' : '#1c1c1e', index: font.index || 0 })
        .then((r) => { if (alive) { got = r.image; setImg(r); setErr(false); } else releaseImage(r.image); }).catch(() => alive && setErr(true));
    }, 60);
    return () => { alive = false; clearTimeout(id); if (got) releaseImage(got); };
  }, [font?.path, font?.index, text, size, width, th.mode, extra?.join(',')]);
  if (err) return <Text style={{ fontSize: 12, color: '#ff453a' }}>{tr('fonts.cantRender')}</Text>;
  if (!img) return <View style={{ height: size * 1.4 }} />;
  return <DocImage image={img.image} style={{ width: img.w, height: img.h }} />;
}

function CharMap({ font, ranges, th }) {
  const blocks = useMemo(() => BLOCKS.filter(([, a, b]) => ranges.some(([x, y]) => x <= b && y >= a)), [ranges]);
  const [block, setBlock] = useState(0);
  const [img, setImg] = useState(null);
  const [pick, setPick] = useState(null);
  const [chunk, setChunk] = useState(0);
  const gridRef = useRef(null);
  const cur = blocks[Math.min(block, blocks.length - 1)];
  const CELL = 40, COLS = 16;
  const cps = useMemo(() => {
    if (!cur) return [];
    const out = [];
    for (let c = cur[1] + chunk * 256; c <= cur[2] && out.length < 256; c++) if (ranges.some(([x, y]) => c >= x && c <= y)) out.push(c);
    return out;
  }, [cur, chunk, ranges]);
  useEffect(() => { setChunk(0); setPick(null); }, [block]);
  useEffect(() => {
    if (!cps.length) { setImg(null); return undefined; }
    let alive = true, got = null;
    fontGrid(font.path, { cps, cell: CELL, cols: COLS, fg: th.mode === 'dark' ? '#f2f2f7' : '#1c1c1e', index: font.index || 0 })
      .then((r) => { if (alive) { got = r.image; setImg(r); } else releaseImage(r.image); }).catch(() => {});
    return () => { alive = false; if (got) releaseImage(got); };
  }, [font?.path, cps.join(','), th.mode]);
  if (!blocks.length) return <Text style={{ margin: 16, fontSize: 12, color: th.textDim }}>{tr('fonts.noChars')}</Text>;
  const size = cur ? cur[2] - cur[1] + 1 : 0;
  const pages = Math.ceil(size / 256);
  return (
    <View style={{ flex: 1, flexDirection: 'row', gap: 16 }}>
      <View style={{ width: 200 }}>
        <View style={{ flex: 1, overflow: 'hidden' }}>
          {blocks.slice(0, 40).map((b, i) => (
            <View key={b[0]} onClick={() => setBlock(i)} style={{ height: 26, paddingHorizontal: 10, justifyContent: 'center', borderRadius: 6, backgroundColor: i === block ? th.accent : 'transparent' }}>
              <Text style={{ fontSize: 13, color: i === block ? '#fff' : th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{tr(`fonts.block.${b[0]}`)}</Text>
            </View>
          ))}
        </View>
      </View>
      <View style={{ flex: 1, gap: 8 }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          <Text style={{ flex: 1, fontSize: 11, color: th.textDim }}>{cur ? `U+${cur[1].toString(16).toUpperCase().padStart(4, '0')}–U+${cur[2].toString(16).toUpperCase().padStart(4, '0')}` : ''}</Text>
          {pages > 1 && <Text style={{ fontSize: 11, color: th.textDim }}>{`${chunk + 1}/${pages}`}</Text>}
          {pages > 1 && <Button label="‹" onClick={() => setChunk((c) => Math.max(0, c - 1))} />}
          {pages > 1 && <Button label="›" onClick={() => setChunk((c) => Math.min(pages - 1, c + 1))} />}
        </View>
        <View style={{ direction: 'ltr' }} onMouseDown={(ev) => {
          if (!img || !gridRef.current) return;
          const [gx, gy] = lord.getLayout(gridRef.current.id);
          const i = Math.floor((ev.y - gy) / CELL) * COLS + Math.floor((ev.x - gx) / CELL);
          if (i >= 0 && i < cps.length && img.data.present?.[i]) setPick(cps[i]);
        }}>
          <View ref={gridRef} style={{ width: CELL * COLS, borderRadius: 8, backgroundColor: th.mode === 'dark' ? '#1c1c20' : '#ffffff', border: `1px solid ${th.separator}` }}>
            {img ? <DocImage image={img.image} style={{ width: img.w, height: img.h }} /> : <View style={{ height: 200 }} />}
          </View>
        </View>
        {pick != null && (
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, paddingVertical: 8, paddingHorizontal: 12, borderRadius: 10, backgroundColor: th.control }}>
            <Text style={{ fontSize: 28, color: th.text, minWidth: 40, textAlign: 'center' }}>{String.fromCodePoint(pick)}</Text>
            <Text style={{ flex: 1, fontSize: 12, color: th.textDim }}>{`U+${pick.toString(16).toUpperCase().padStart(4, '0')}`}</Text>
            <Button label={tr('fonts.copy')} onClick={() => clipboard.set(String.fromCodePoint(pick))} />
          </View>
        )}
      </View>
    </View>
  );
}

export default function Fonts() {
  const th = useTheme();
  const { id: wid, args } = useAppWindow();
  const [fams, setFams] = useState(() => loadFonts(false));
  const [coll, setColl] = useState('all');
  const [q, setQ] = useState('');
  const [sel, setSel] = useState(null);           // family or { file }
  const [styleIdx, setStyleIdx] = useState(0);
  const [tab, setTab] = useState('sample');
  const [text, setText] = useState('');
  const [size, setSize] = useState(36);
  const [info, setInfo] = useState(null);
  const [msg, setMsg] = useState(null);
  const [width, setWidth] = useState(600);
  const pane = useRef(null);
  const [file, setFile] = useState(() => (args?.path && FONT_EXT.test(args.path) ? args.path : null));
  useEffect(() => { if (args?.path && args?.nonce && FONT_EXT.test(args.path)) setFile(args.path); }, [args?.nonce]);
  useEffect(() => { if (!msg) return undefined; const id = setTimeout(() => setMsg(null), 4000); return () => clearTimeout(id); }, [msg]);
  useEffect(() => { const id = setInterval(() => { if (pane.current) { const w = lord.getLayout(pane.current.id)[2] - 48; if (Math.abs(w - width) > 8) setWidth(w); } }, 300); return () => clearInterval(id); }, [width]);

  const visible = fams.filter((f) => (coll === 'all' || (coll === 'user' && f.user) || (coll === 'mono' && f.mono) || (coll === 'color' && f.color))
    && (!q || f.family.toLowerCase().includes(q.toLowerCase())));
  // a file opened from Files (not installed yet) is shown as one more family
  const [fileFont, setFileFont] = useState(null);
  useEffect(() => {
    if (!file) { setFileFont(null); return; }
    fontInfo(file).then((r) => { setFileFont({ family: r.family, styles: [{ family: r.family, style: r.style, path: file, index: 0, mono: r.mono }], file: true, info: r }); setSel('file'); setTab('sample'); })
      .catch(() => { setMsg(tr('fonts.invalid')); setFile(null); });
  }, [file]);
  const fam = fileFont && (sel === 'file' || (!sel && file)) ? fileFont : visible.find((f) => f.family === sel) || (sel ? null : visible[0]);
  const font = fam?.styles[Math.min(styleIdx, fam.styles.length - 1)];
  useEffect(() => { setStyleIdx(0); }, [fam?.family]);
  useEffect(() => {
    setInfo(null);
    if (!font) return;
    fontInfo(font.path, font.index || 0).then(setInfo).catch(() => setInfo({ error: true }));
  }, [font?.path, font?.index]);
  useEffect(() => { wm.setTitle(wid, fam ? fam.family : tr('apps.fonts')); }, [fam?.family]);

  const sampleText = text || tr('fonts.pangram');
  const installed = fam && !fam.file;
  // a file whose family and style are already installed (fonts.c does not load the same font twice)
  const already = fam?.file && fams.some((f) => f.family === fam.family && f.styles.some((st) => st.style === fam.styles[0].style));
  const install = () => {
    if (!file) return;
    try {
      try { fs.mkdir('.local/share'); } catch (e) { /* already there */ }
      try { fs.mkdir('.local/share/fonts'); } catch (e) { /* already there */ }
      const dest = `${userDir()}/${fs.basename(file)}`;
      try { fs.stat(dest); setMsg(tr('fonts.already')); return; } catch (e) { /* free */ }
      fs.copy(file, dest);
      // the copy runs in a thread: rescan when it finishes
      const t0 = Date.now();
      const id = setInterval(() => {
        try { fs.stat(dest); } catch (e) { if (Date.now() - t0 < 5000) return; }
        clearInterval(id);
        const next = loadFonts(true);
        setFams(next);
        setFile(null);
        setSel(fileFont?.family || null);
        setMsg(tr('fonts.installed', { name: fileFont?.family || fs.basename(file) }));
      }, 200);
    } catch (e) { setMsg(tr('fonts.error', { error: e.message })); }
  };
  const remove = () => {
    if (!fam || !fam.user) return;
    let n = 0;
    for (const s of fam.styles) if (isUser(s.path)) { try { fs.trash(s.path); n++; } catch (e) { setMsg(tr('fonts.error', { error: e.message })); } }
    if (n) { setFams(loadFonts(true)); setSel(null); setMsg(tr('fonts.removed', { name: fam.family })); }
  };
  useKeyboard((e) => {
    if ((e.meta || e.ctrl) && (e.base || '').toLowerCase() === 'f') { /* the search is already in view */ }
  });

  const colls = [['all', tr('fonts.coll.all')], ['user', tr('fonts.coll.user')], ['mono', tr('fonts.coll.mono')], ['color', tr('fonts.coll.color')]];
  return (
    <View style={{ flex: 1, flexDirection: 'row' }}>
      <View style={{ width: 230, backgroundColor: th.sidebarBg, paddingTop: 12, paddingHorizontal: 10, gap: 2 }}>
        <View style={{ position: 'absolute', right: 0, top: 0, bottom: 0, width: 1, backgroundColor: th.separator }} />
        <SearchField value={q} onChange={setQ} placeholder={tr('fonts.search')} style={{ marginBottom: 6 }} />
        <SectionTitle>{tr('fonts.collections')}</SectionTitle>
        {colls.map(([k, l]) => (
          <SidebarItem key={k} label={l} selected={coll === k} onClick={() => { setColl(k); setSel(null); }} />
        ))}
        <View style={{ height: 6 }} />
        <SectionTitle>{tr('fonts.families', { count: visible.length })}</SectionTitle>
        <FamilyList list={visible} fileFont={fileFont} current={fam} onPick={(f) => setSel(f.file ? 'file' : f.family)} th={th} />
      </View>
      <View ref={pane} style={{ flex: 1, paddingVertical: 20, paddingHorizontal: 24, gap: 14 }}>
        {!fam ? <EmptyState icon="A" title={tr('fonts.none')} /> : (
          <>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12 }}>
              <View style={{ flex: 1, gap: 2, overflow: 'hidden' }}>
                <Text style={{ fontSize: 22, fontWeight: 600, color: th.text, whiteSpace: 'nowrap' }}>{fam.family}</Text>
                <Text style={{ fontSize: 12, color: th.textDim }}>{fam.file ? tr('fonts.notInstalled') : tr('fonts.styles', { count: fam.styles.length })}</Text>
              </View>
              {fam.file && !already ? <Button primary label={tr('fonts.install')} onClick={install} /> : null}
              {fam.file && already ? <Text style={{ fontSize: 12, color: th.textDim }}>{tr('fonts.already')}</Text> : null}
              {installed && fam.user ? <Button label={tr('fonts.remove')} onClick={remove} /> : null}
            </View>
            {fam.styles.length > 1 && (
              <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6 }}>
                {fam.styles.map((s, i) => (
                  <View key={`${s.path}#${s.index}`} onClick={() => setStyleIdx(i)} style={{ paddingHorizontal: 12, height: 24, borderRadius: 12, justifyContent: 'center', backgroundColor: i === styleIdx ? th.accent : th.control }}>
                    <Text style={{ fontSize: 12, color: i === styleIdx ? '#fff' : th.text }}>{s.style}</Text>
                  </View>
                ))}
              </View>
            )}
            <Segmented appearance="neutral" value={tab} onChange={setTab} style={{ alignSelf: 'center' }}
              options={[['sample', tr('fonts.tab.sample')], ['chars', tr('fonts.tab.chars')], ['info', tr('fonts.tab.info')]]} />
            {tab === 'sample' && (
              <View style={{ flex: 1, gap: 12 }}>
                <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12 }}>
                  <TextInput value={text} onChange={setText} placeholder={tr('fonts.typeHere')} style={{ flex: 1 }} />
                  <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, direction: 'ltr' }}>
                    <Text style={{ fontSize: 11, color: th.textDim }}>A</Text>
                    <Slider value={size} min={8} max={120} onChange={(v) => setSize(Math.round(v))} width={120} />
                    <Text style={{ fontSize: 17, color: th.textDim }}>A</Text>
                  </View>
                  <Text style={{ width: 48, fontSize: 12, color: th.textDim, textAlign: 'end' }}>{`${size} px`}</Text>
                </View>
                <View style={{ flex: 1, overflow: 'hidden', borderRadius: 10, paddingVertical: 14, paddingHorizontal: 16, backgroundColor: th.mode === 'dark' ? '#1c1c20' : '#ffffff', border: `1px solid ${th.separator}` }}>
                  <Sample font={font} text={sampleText} size={size} width={width - 36} th={th} />
                  <View style={{ height: 1, backgroundColor: th.separator, marginVertical: 14 }} />
                  <Sample font={font} text={tr('fonts.alphabet')} size={18} width={width - 36} th={th} extra={[12, 18, 24, 36]} />
                </View>
              </View>
            )}
            {tab === 'chars' && info && !info.error && <CharMap font={font} ranges={info.ranges || []} th={th} />}
            {tab === 'info' && (
              <View style={{ gap: 4 }}>
                {info?.error ? <Text style={{ fontSize: 12, color: '#ff453a' }}>{tr('fonts.invalid')}</Text> : info && <GroupBox>{[
                  [tr('fonts.info.family'), info.family], [tr('fonts.info.style'), info.style], [tr('fonts.info.ps'), info.ps],
                  [tr('fonts.info.glyphs'), String(info.glyphs)], [tr('fonts.info.chars'), String(info.chars)],
                  [tr('fonts.info.kind'), [info.mono ? tr('fonts.coll.mono') : null, info.color ? tr('fonts.coll.color') : null, info.scalable ? tr('fonts.scalable') : tr('fonts.bitmap')].filter(Boolean).join(' · ')],
                  [tr('fonts.info.file'), font.path], [tr('fonts.info.size'), (() => { try { return formatBytes(fs.stat(font.path).size); } catch (e) { return '—'; } })()],
                ].map(([k, v]) => <FormRow key={k} label={k} labelWidth={150} minHeight={32}>{v}</FormRow>)}</GroupBox>}
              </View>
            )}
          </>
        )}
        {msg ? <Text style={{ fontSize: 12, color: th.accent }}>{msg}</Text> : null}
      </View>
    </View>
  );
}

function FamilyList({ list, fileFont, current, onPick, th }) {
  const [offset, setOffset] = useState(0);
  const all = fileFont ? [fileFont, ...list] : list;
  const ROW = 26;
  const from = Math.floor(offset / ROW);
  return (
    <View onWheel={(e) => setOffset((o) => Math.max(0, Math.min(Math.max(0, all.length * ROW - 300), o + e.deltaY)))} style={{ flex: 1, overflow: 'hidden' }}>
      {all.slice(from, from + 40).map((f, i) => {
        const on = current && current.family === f.family && !!current.file === !!f.file;
        return (
          <View key={`${f.file ? 'file:' : ''}${f.family}`} onClick={() => onPick(f)} style={{ position: 'absolute', left: 0, right: 0, top: (from + i) * ROW - offset, height: ROW, paddingHorizontal: 10,
            flexDirection: 'row', alignItems: 'center', gap: 6, borderRadius: 6, backgroundColor: on ? th.accent : 'transparent' }}>
            <Text style={{ flex: 1, fontSize: 13, color: on ? '#fff' : th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{f.family}</Text>
            {f.file ? <Text style={{ fontSize: 10, color: on ? '#fff' : th.accent }}>{tr('fonts.new')}</Text> : f.user ? <Text style={{ fontSize: 10, color: on ? '#fff' : th.textDim }}>●</Text> : null}
          </View>
        );
      })}
    </View>
  );
}
