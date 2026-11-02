// printdialog.jsx — the system print dialog (print.open in print.js). It is mounted by
// startShell, so any app uses it without drawing anything of its own. The preview is the same
// confined PDF reader (axia-doc): what you see is what is sent to CUPS.
import { useEffect, useMemo, useRef, useState } from 'react';
import { View, Text, addListener, mainBox } from '@lordos/runtime';
import { useStore } from './store.js';
import { useTheme } from './theme.js';
import { t, locale } from './i18n.js';
import { Button, Segmented, Stepper, TextInput, ScrollView } from './components.jsx';
import { print, printRequest } from './print.js';
import { pdf } from './pdf.js';
import { DocImage, releaseImage, releaseBlob, saveBlob, makeTextPdf, makeImagePdf } from './doc.js';

const MEDIA = { a4: 'iso_a4_210x297mm', letter: 'na_letter_8.5x11in', legal: 'na_legal_8.5x14in', a5: 'iso_a5_148x210mm' };
const PDF_DEST = '\u0000pdf';
// countries that use letter paper
const LETTER = new Set(['US', 'CA', 'MX', 'PH', 'CO', 'VE', 'CL', 'CR', 'GT', 'PR', 'SV', 'NI', 'PA', 'DO']);                 // «Save as PDF» (not a CUPS queue)

function parsePages(s, n) {
  const out = [];
  for (const part of String(s).split(',')) {
    const m = /^\s*(\d+)\s*(?:-\s*(\d+))?\s*$/.exec(part);
    if (!m) { if (part.trim()) return null; continue; }
    const a = Number(m[1]), b = m[2] ? Number(m[2]) : a;
    if (a < 1 || b < a) return null;
    for (let p = a; p <= Math.min(b, n); p++) out.push(p - 1);
  }
  return out;
}

function stateLabel(p) {
  if (!p) return '';
  if (p.state === 5) return t('print.state.stopped');
  if (p.state === 4) return t('print.state.printing');
  if (!p.accepting) return t('print.state.rejecting');
  return t('print.state.idle');
}

function Preview({ doc, pages, th }) {
  const [i, setI] = useState(0);
  const [img, setImg] = useState(null);
  const list = pages && pages.length ? pages : doc ? [...Array(doc.info.pages).keys()] : [];
  const page = list[Math.min(i, list.length - 1)];
  useEffect(() => { setI(0); }, [doc, pages?.join(',')]);
  useEffect(() => {
    if (!doc || page == null) return undefined;
    let alive = true, got = null;
    doc.render(page, 520, { prio: 8 }).then((r) => { if (alive) { got = r.image; setImg(r); } else releaseImage(r.image); }).catch(() => {});
    return () => { alive = false; if (got) releaseImage(got); };
  }, [doc, page]);
  const size = doc?.info.sizes[page] || [595, 842];
  const W = 250, H = Math.round(W * size[1] / size[0]);
  const fit = H > 330 ? 330 / H : 1;
  return (
    <View style={{ width: 280, alignItems: 'center', gap: 8, paddingVertical: 10 }}>
      <View style={{ width: W * fit, height: H * fit, backgroundColor: '#ffffff', boxShadow: '0 4px 14px rgba(0,0,0,0.35)', border: `1px solid ${th.separator}` }}>
        {img && <DocImage image={img.image} style={{ width: W * fit, height: H * fit, objectFit: 'contain' }} />}
        {!img && <Text style={{ margin: 20, fontSize: 11, color: '#888', textAlign: 'center' }}>{t('print.preparing')}</Text>}
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, direction: 'ltr' }}>
        <View onClick={() => setI((v) => Math.max(0, v - 1))} style={{ paddingHorizontal: 8, opacity: i > 0 ? 1 : 0.3 }}><Text style={{ fontSize: 15, color: th.text }}>‹</Text></View>
        <Text style={{ fontSize: 11, color: th.textDim }}>{list.length ? t('print.pageOf', { page: i + 1, count: list.length }) : ''}</Text>
        <View onClick={() => setI((v) => Math.min(list.length - 1, v + 1))} style={{ paddingHorizontal: 8, opacity: i < list.length - 1 ? 1 : 0.3 }}><Text style={{ fontSize: 15, color: th.text }}>›</Text></View>
      </View>
    </View>
  );
}

function Row({ label, children, th }) {
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, minHeight: 30 }}>
      <Text style={{ width: 110, fontSize: 12, color: th.textDim, textAlign: 'end' }}>{label}</Text>
      <View style={{ flex: 1, flexDirection: 'row', alignItems: 'center', gap: 8 }}>{children}</View>
    </View>
  );
}

function Dialog({ req }) {
  const th = useTheme();
  const src = req.source || {};
  const [printers, setPrinters] = useState(null);
  const [error, setError] = useState(null);
  const [dest, setDest] = useState(null);
  const [copies, setCopies] = useState(1);
  const [range, setRange] = useState('all');
  const [pagesText, setPagesText] = useState('');
  const [color, setColor] = useState('color');
  const [duplex, setDuplex] = useState('one-sided');
  const [paper, setPaper] = useState(() => (LETTER.has(String(locale.get().region || '').split('_')[1] || '') ? 'letter' : 'a4'));
  const [doc, setDoc] = useState(null);
  const [blob, setBlob] = useState(null);
  const [busy, setBusy] = useState(false);
  const docRef = useRef(null), blobRef = useRef(null);

  useEffect(() => {
    print.printers().then((r) => {
      setPrinters(r.printers);
      setDest(r.default && r.printers.some((p) => p.name === r.default) ? r.default : r.printers[0]?.name || PDF_DEST);
    }).catch((e) => { setPrinters([]); setDest(PDF_DEST); setError(e.code === 'no-cups' ? t('print.noCups') : null); });
  }, []);

  // the PDF that will be printed (and the preview)
  const needsPaper = !!(src.text != null || src.image);
  useEffect(() => {
    let alive = true;
    (async () => {
      try {
        let b = null;
        if (src.text != null) b = await makeTextPdf({ text: src.text, title: src.title || '', mono: !!src.mono, paper });
        else if (src.image) b = await makeImagePdf(src.image, { paper });
        else if (src.doc) b = await src.doc.blob();
        const d = await pdf.open(b ? { blob: b.blob } : src.path, { quick: false });
        if (!alive) { d.close(); if (b) releaseBlob(b.blob); return; }
        docRef.current?.close();
        if (blobRef.current) releaseBlob(blobRef.current);
        docRef.current = d;
        blobRef.current = b ? b.blob : null;
        setDoc(d);
        setBlob(b ? b.blob : null);
      } catch (e) { if (alive) setError(t('print.cantPrepare')); }
    })();
    return () => { alive = false; };
  }, [needsPaper ? paper : '']);
  useEffect(() => () => { docRef.current?.close(); if (blobRef.current) releaseBlob(blobRef.current); }, []);

  const finish = (result) => { printRequest.set(null); req.resolve(result); };
  useEffect(() => addListener('keydown', (e) => { if (e.key === 'Escape') finish(null); }), []);

  const printer = printers?.find((p) => p.name === dest);
  const n = doc?.info.pages || 0;
  const pages = range === 'some' ? parsePages(pagesText, n) : null;
  const badPages = range === 'some' && (!pages || !pages.length);
  const hasColor = printer ? printer.color || (printer.colorModes || []).includes('color') : true;
  const hasDuplex = printer && (printer.sides || []).some((s) => s.startsWith('two-sided'));

  async function go() {
    if (!doc || busy || badPages) return;
    setBusy(true);
    try {
      if (dest === PDF_DEST) {
        const dir = `${lord.fs.home()}/${['Documentos', 'Documents'].find((d) => { try { return lord.fs.stat(d).dir; } catch (e) { return false; } }) || ''}`.replace(/\/$/, '');
        const base = String(src.title || t('print.untitled')).replace(/\.[^.]+$/, '').replace(/[/\\\u0000-\u001f]/g, '_').slice(0, 80) || 'pdf';
        let name = `${base}.pdf`;
        for (let i = 2; i < 500; i++) { try { lord.fs.stat(`${dir}/${name}`); name = `${base} ${i}.pdf`; } catch (e) { break; } }
        const out = `${dir}/${name}`;
        if (blob) saveBlob(blob, out);
        else lord.fs.copy(src.path, out, false);
        finish({ saved: out });
        return;
      }
      const opts = { name: src.title || '', copies, color: hasColor ? color : 'monochrome', media: MEDIA[paper] };
      if (pages) opts.pages = pagesText;
      if (hasDuplex) opts.sides = duplex;
      const r = await print.submit(dest, blob || src.path, opts);
      finish({ job: r.job, printer: dest });
    } catch (e) {
      setBusy(false);
      setError(e.code === 'bad-pages' ? t('print.badPages') : t('print.failed', { error: e.message }));
    }
  }

  const destOptions = [...(printers || []).map((p) => ({ id: p.name, label: p.info || p.name, sub: stateLabel(p) })), { id: PDF_DEST, label: t('print.saveAsPdf'), sub: '' }];
  return (
    <View style={{ position: 'absolute', ...mainBox(), backgroundColor: 'rgba(0,0,0,0.35)', alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: 720, borderRadius: 14, backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, boxShadow: '0 20px 50px rgba(0,0,0,0.5)', overflow: 'hidden' }}>
        <View style={{ height: 40, justifyContent: 'center', paddingHorizontal: 16 }}>
          <Text style={{ fontSize: 14, fontWeight: 700, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{src.title ? t('print.titleOf', { title: src.title }) : t('print.title')}</Text>
        </View>
        <View style={{ height: 1, backgroundColor: th.separator }} />
        <View style={{ flexDirection: 'row' }}>
          <View style={{ backgroundColor: th.mode === 'dark' ? 'rgba(0,0,0,0.2)' : 'rgba(0,0,0,0.04)' }}>
            <Preview doc={doc} pages={pages} th={th} />
          </View>
          <View style={{ flex: 1, padding: 14, gap: 6 }}>
            <Row th={th} label={t('print.printer')}>
              <View style={{ flex: 1, height: 120, borderRadius: 7, border: `1px solid ${th.separator}`, backgroundColor: th.inputBg }}>
                <ScrollView style={{ flex: 1 }}>
                  {printers == null ? <Text style={{ margin: 8, fontSize: 12, color: th.textDim }}>{t('print.searching')}</Text>
                    : destOptions.map((o) => (
                      <View key={o.id} onClick={() => setDest(o.id)} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, height: 28, paddingHorizontal: 8,
                        backgroundColor: dest === o.id ? th.accent : 'transparent' }}>
                        <Text style={{ fontSize: 13 }}>{o.id === PDF_DEST ? '📄' : '🖨'}</Text>
                        <Text style={{ flex: 1, fontSize: 12, color: dest === o.id ? '#fff' : th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{o.label}</Text>
                        <Text style={{ fontSize: 11, color: dest === o.id ? '#fff' : th.textDim }}>{o.sub}</Text>
                      </View>
                    ))}
                </ScrollView>
              </View>
            </Row>
            {printer?.message ? <Row th={th} label=""><Text style={{ fontSize: 11, color: th.textDim }}>{printer.message}</Text></Row> : null}
            <Row th={th} label={t('print.copies')}><Stepper value={copies} min={1} max={99} onChange={setCopies} /></Row>
            <Row th={th} label={t('print.pages')}>
              <Segmented value={range} onChange={setRange} options={[['all', t('print.allPages', { count: n })], ['some', t('print.range')]]} />
              {range === 'some' && <TextInput value={pagesText} onChange={setPagesText} placeholder="1-3, 5" style={{ width: 110, height: 24 }} />}
            </Row>
            {dest !== PDF_DEST && (
              <Row th={th} label={t('print.color')}>
                {hasColor ? <Segmented value={color} onChange={setColor} options={[['color', t('print.colorOn')], ['monochrome', t('print.bw')]]} />
                  : <Text style={{ fontSize: 12, color: th.textDim }}>{t('print.bwOnly')}</Text>}
              </Row>
            )}
            {dest !== PDF_DEST && hasDuplex && (
              <Row th={th} label={t('print.duplex')}>
                <Segmented value={duplex} onChange={setDuplex} options={[['one-sided', t('print.oneSided')], ['two-sided-long-edge', t('print.longEdge')], ['two-sided-short-edge', t('print.shortEdge')]]} />
              </Row>
            )}
            <Row th={th} label={t('print.paper')}>
              <Segmented value={paper} onChange={setPaper} options={Object.keys(MEDIA).map((k) => [k, t(`print.paper.${k}`)])} />
            </Row>
            <View style={{ flex: 1 }} />
            {error ? <Text style={{ fontSize: 12, color: '#ff453a' }}>{error}</Text> : null}
            {badPages ? <Text style={{ fontSize: 12, color: '#ff9f0a' }}>{t('print.badPages')}</Text> : null}
            <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 6 }}>
              <Button label={t('print.cancel')} onClick={() => finish(null)} />
              <Button primary label={busy ? t('print.sending') : dest === PDF_DEST ? t('print.save') : t('print.print')} onClick={go} />
            </View>
          </View>
        </View>
      </View>
    </View>
  );
}

export function PrintDialog() {
  const req = useStore(printRequest);
  return useMemo(() => (req ? <Dialog req={req} /> : null), [req]);
}
