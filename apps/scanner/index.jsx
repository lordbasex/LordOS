// Scanner: "driverless" network scanners (eSCL / AirScan) with our own client, without SANE:
// preview, a hand-drawn area, resolution, color or grayscale, and save as PDF, PNG or JPEG. What
// the scanner sends is decoded and converted in the sandboxed axia-doc helper.
import {
  View, Text, Button, Segmented, Hairline, EmptyState, ScrollView, useTheme, useAppWindow, wm, fs, print, DocImage, releaseImage, decodeImage, convertImage,
  formatDate, formatTime, typeScale, t as tr,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(160deg, #66d4cf, #0f7c78)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, overflow: 'hidden' }}>
      {/* scanner bed with the lid and the passing light */}
      <View style={{ position: 'absolute', left: s * 0.12, top: s * 0.46, width: s * 0.76, height: s * 0.3, borderRadius: s * 0.06, backgroundColor: '#e9f7f6' }} />
      <View style={{ position: 'absolute', left: s * 0.12, top: s * 0.3, width: s * 0.76, height: s * 0.12, borderRadius: s * 0.05, backgroundColor: '#cfe9e7' }} />
      <View style={{ position: 'absolute', left: s * 0.18, top: s * 0.54, width: s * 0.64, height: s * 0.05, backgroundColor: '#7cf5a0' }} />
      <View style={{ position: 'absolute', left: s * 0.18, top: s * 0.64, width: s * 0.3, height: s * 0.04, backgroundColor: '#9fbfbd' }} />
    </View>
  );
}

const COLORS = { RGB24: 'color', Grayscale8: 'gray', BlackAndWhite1: 'bw' };

function destDir(kind) {
  const home = fs.home();
  const names = kind === 'pdf' ? ['Documentos', 'Documents'] : ['Imágenes', 'Pictures', 'Imagens'];
  for (const n of names) { try { if (fs.stat(`${home}/${n}`).dir) return `${home}/${n}`; } catch (e) { /* not there */ } }
  return home;
}

export default function Scanner() {
  const th = useTheme();
  const { args } = useAppWindow();
  const [scanner, setScanner] = useState(args?.scanner || null);
  const [found, setFound] = useState(null);
  const [caps, setCaps] = useState(null);
  const [error, setError] = useState(null);
  const [color, setColor] = useState('RGB24');
  const [res, setRes] = useState(150);
  const [format, setFormat] = useState('pdf');
  const [source, setSource] = useState('Platen');
  const [preview, setPreview] = useState(null);      // { image, w, h }
  const [area, setArea] = useState(null);            // [x, y, w, h] as fractions of the preview
  const [busy, setBusy] = useState(null);
  const [msg, setMsg] = useState(null);
  const box = useRef(null);
  const drag = useRef(null);

  useEffect(() => { if (args?.scanner && args?.nonce) setScanner(args.scanner); }, [args?.nonce]);
  useEffect(() => {
    if (scanner) return;
    print.discover().then((r) => setFound(r.scanners.filter((s) => !s.tls && s.lan))).catch(() => setFound([]));
  }, [scanner]);
  useEffect(() => {
    if (!scanner) return;
    setCaps(null); setError(null);
    print.scanCaps(scanner).then((c) => {
      setCaps(c);
      if (c.res.length) setRes(c.res.includes(300) ? 300 : c.res.includes(150) ? 150 : c.res[0]);
      if (c.colors.length && !c.colors.includes('RGB24')) setColor(c.colors[0]);
      if (!c.platen && c.adf) setSource('Feeder');
    }).catch((e) => setError(tr('scanner.unreachable', { error: e.message })));
  }, [scanner]);
  useEffect(() => () => { if (preview) releaseImage(preview.image); }, [preview]);

  const formats = caps ? [...new Set([...(caps.formats || []), ...(caps.formats2 || [])])] : [];
  const imgFormat = formats.includes('image/jpeg') ? 'image/jpeg' : formats.includes('image/png') ? 'image/png' : formats[0] || 'image/jpeg';
  const region = () => {
    const W = caps?.maxW || 2550, H = caps?.maxH || 3508;
    const [x, y, w, h] = area || [0, 0, 1, 1];
    return { x: Math.round(x * W), y: Math.round(y * H), w: Math.max(30, Math.round(w * W)), h: Math.max(30, Math.round(h * H)) };
  };

  const doPreview = async () => {
    setBusy('preview'); setError(null);
    try {
      const low = caps?.res.length ? Math.min(...caps.res.filter((r) => r >= 50)) : 75;
      const r = await print.scan(scanner, { source, color: 'RGB24', res: low, format: imgFormat, x: 0, y: 0, w: caps?.maxW || 2550, h: caps?.maxH || 3508 });
      const img = await decodeImage(r.file, { maxw: 520, maxh: 560 });
      setPreview(img);
      setArea(null);
    } catch (e) { setError(tr('scanner.failed', { error: e.message })); }
    setBusy(null);
  };

  const doScan = async () => {
    setBusy('scan'); setError(null);
    try {
      const direct = format === 'pdf' ? formats.includes('application/pdf') : format === 'png' ? formats.includes('image/png') : formats.includes('image/jpeg');
      const want = direct ? (format === 'pdf' ? 'application/pdf' : `image/${format === 'png' ? 'png' : 'jpeg'}`) : imgFormat;
      const r = await print.scan(scanner, { source, color, res, format: want, ...region() });
      const d = new Date();
      const base = tr('scanner.fileName', { date: formatDate(d, 'numeric').replace(/\//g, '-'), time: formatTime(d).replace(/:/g, '.') });
      const dir = destDir(format);
      const ext = format === 'jpeg' ? 'jpg' : format;
      let name = `${base}.${ext}`;
      for (let i = 2; i < 100; i++) { try { fs.stat(`${dir}/${name}`); name = `${base} ${i}.${ext}`; } catch (e) { break; } }
      const out = `${dir}/${name}`;
      if (direct) fs.copy(r.file, out);
      else await convertImage(r.file, out, { format: format === 'jpeg' ? 'jpeg' : format, native: true, dpi: res });
      setMsg(tr('scanner.saved', { name }));
      lastSaved.current = out;
    } catch (e) { setError(tr('scanner.failed', { error: e.message })); }
    setBusy(null);
  };
  const lastSaved = useRef(null);

  // area: drag on the preview
  const onDown = (ev) => {
    if (!preview || !box.current) return;
    const [bx, by, bw, bh] = lord.getLayout(box.current.id);
    const fx = (ev.x - bx) / bw, fy = (ev.y - by) / bh;
    drag.current = { bx, by, bw, bh, fx, fy };
    setArea([fx, fy, 0, 0]);
  };
  const onMove = (ev) => {
    const d = drag.current;
    if (!d) return;
    const fx = Math.max(0, Math.min(1, (ev.x - d.bx) / d.bw)), fy = Math.max(0, Math.min(1, (ev.y - d.by) / d.bh));
    setArea([Math.min(fx, d.fx), Math.min(fy, d.fy), Math.abs(fx - d.fx), Math.abs(fy - d.fy)]);
  };
  const onUp = () => { const d = drag.current; drag.current = null; if (d && area && (area[2] < 0.02 || area[3] < 0.02)) setArea(null); };

  if (!scanner) {
    if (!found || !found.length) {
      return <EmptyState icon={<Icon size={56} />} title={tr('scanner.choose')} hint={found == null ? tr('scanner.searching') : tr('scanner.none')} />;
    }
    return (
      <View style={{ flex: 1, padding: 20, gap: 12 }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: th.text }}>{tr('scanner.choose')}</Text>
        <View style={{ borderRadius: 10, border: `1px solid ${th.separator}`, backgroundColor: th.mode === 'dark' ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.025)', paddingHorizontal: 12 }}>
          {found.map((s, i) => (
            <View key={`${s.addr}:${s.port}`} onClick={() => setScanner(s)} style={{ flexDirection: 'row', gap: 12, paddingVertical: 10, alignItems: 'center' }}>
              {i > 0 ? <Hairline edge="top" /> : null}
              <Icon size={32} />
              <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
                <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{s.name}</Text>
                <Text style={{ fontSize: typeScale.caption, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{`${s.model || ''} ${s.addr}:${s.port}`}</Text>
              </View>
              <Text style={{ fontSize: 15, color: th.textDim }}>›</Text>
            </View>
          ))}
        </View>
      </View>
    );
  }
  const sel = area;
  const caption = (text) => <Text style={{ fontSize: typeScale.caption, fontWeight: 600, color: th.textDim, marginTop: 6 }}>{text}</Text>;
  return (
    <View style={{ flex: 1, flexDirection: 'row' }} onMouseMove={onMove} onMouseUp={onUp}>
      <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', padding: 20, backgroundColor: th.mode === 'dark' ? '#2a2a2e' : '#e3e3e8' }}>
        {preview ? (
          <View ref={box} onMouseDown={onDown} style={{ width: preview.w, height: preview.h, backgroundColor: '#fff', boxShadow: '0 2px 10px rgba(0,0,0,0.35)', direction: 'ltr' }}>
            <DocImage image={preview.image} style={{ width: preview.w, height: preview.h }} />
            {sel && sel[2] > 0 && (
              <View style={{ position: 'absolute', left: sel[0] * preview.w, top: sel[1] * preview.h, width: sel[2] * preview.w, height: sel[3] * preview.h,
                border: `2px solid ${th.accent}`, backgroundColor: `${th.accent}22` }} />
            )}
          </View>
        ) : <EmptyState title={busy === 'preview' ? tr('scanner.previewing') : null} hint={busy === 'preview' ? null : tr('scanner.previewHint')} />}
      </View>
      <View style={{ width: 280, backgroundColor: th.sidebarBg }}>
        <Hairline edge="start" />
        <ScrollView style={{ flex: 1, paddingHorizontal: 16, paddingTop: 16 }}>
          <View style={{ gap: 8, paddingBottom: 12 }}>
            <View style={{ gap: 2 }}>
              <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: th.text }}>{scanner.name}</Text>
              <Text style={{ fontSize: typeScale.caption, color: th.textDim }}>{caps ? caps.model || `${scanner.addr}:${scanner.port}` : error ? '' : tr('scanner.connecting')}</Text>
            </View>
            {caps && (
              <>
                {caps.platen && caps.adf ? <Segmented fill value={source} onChange={setSource} options={[['Platen', tr('scanner.platen')], ['Feeder', tr('scanner.feeder')]]} /> : null}
                {caption(tr('scanner.mode'))}
                <Segmented fill value={color} onChange={setColor} options={(caps.colors.length ? caps.colors : ['RGB24']).filter((c) => COLORS[c]).map((c) => [c, tr(`scanner.color.${COLORS[c]}`)])} />
                {caption(tr('scanner.resolution'))}
                <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6 }}>
                  {(caps.res.length ? caps.res : [75, 150, 300, 600]).slice(0, 8).map((r) => (
                    <View key={r} onClick={() => setRes(r)} style={{ paddingHorizontal: 10, height: 24, borderRadius: 12, justifyContent: 'center', backgroundColor: res === r ? th.accent : th.control }}>
                      <Text style={{ fontSize: typeScale.callout, color: res === r ? '#fff' : th.text }}>{tr('scanner.dpi', { dpi: r })}</Text>
                    </View>
                  ))}
                </View>
                {caption(tr('scanner.format'))}
                <Segmented fill value={format} onChange={setFormat} options={[['pdf', 'PDF'], ['png', 'PNG'], ['jpeg', 'JPEG']]} />
                <Text style={{ fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.4, marginTop: 4 }}>{area ? tr('scanner.areaSet', { w: Math.round(region().w / 300 * 25.4), h: Math.round(region().h / 300 * 25.4) }) : tr('scanner.areaAll')}</Text>
              </>
            )}
            {error ? <Text style={{ fontSize: typeScale.callout, color: '#ff453a', lineHeight: 1.4 }}>{error}</Text> : null}
            {msg ? <Text style={{ fontSize: typeScale.callout, color: th.accent, lineHeight: 1.4 }}>{msg}</Text> : null}
          </View>
        </ScrollView>
        {/* the actions sit in a fixed footer of the panel, above the Dock */}
        <View style={{ paddingHorizontal: 16, paddingTop: 12, paddingBottom: 16, gap: 10 }}>
          <Hairline edge="top" />
          <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'flex-end', flexWrap: 'wrap' }}>
            {lastSaved.current && format !== 'pdf' ? <Button label={tr('scanner.print')} onClick={() => print.open({ title: fs.basename(lastSaved.current), image: lastSaved.current })} /> : null}
            <Button label={busy === 'preview' ? tr('scanner.working') : tr('scanner.preview')} onClick={() => !busy && caps && doPreview()} />
            <Button primary label={busy === 'scan' ? tr('scanner.working') : tr('scanner.scan')} onClick={() => !busy && caps && doScan()} />
          </View>
          <View onClick={() => { setScanner(null); setCaps(null); setPreview(null); }} style={{ alignSelf: 'flex-start' }}>
            <Text style={{ fontSize: typeScale.callout, color: th.accent }}>{tr('scanner.other')}</Text>
          </View>
        </View>
      </View>
    </View>
  );
}
