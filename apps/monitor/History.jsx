// History.jsx — the Network tab's traffic history (0.8.2, docs/MONITOR.md "Traffic history"): the
// same mirrored chart as the live minute, over 1 hour, 24 hours, 7 or 30 days, read from the file
// axia-server keeps (monitor.history). Step back and forward one window at a time; "Now" comes back
// to the present. The file holds bytes per minute (7 days) and per hour (30 days), for the machine
// and for each app; never an address or a name.
import { View, Text, Segmented, useTheme, useLocale, monitor, formatDate, formatTime } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { TrafficChart } from './TrafficChart.jsx';

// window and bucket of each zoom: about 60–170 bars, like the live minute
export const RANGES = {
  '1h': { span: 3600, step: 60 },
  '24h': { span: 86400, step: 900 },
  '7d': { span: 7 * 86400, step: 3600 },
  '30d': { span: 30 * 86400, step: 6 * 3600 },
};

// end: Unix seconds of the window's end, or null for "now". The window is aligned to its buckets.
export function windowOf(range, end) {
  const r = RANGES[range];
  if (!r) return null;
  const now = Math.floor(Date.now() / 1000);
  const e = Math.ceil((end ?? now) / r.step) * r.step;
  return { from: e - r.span, step: r.step, n: Math.round(r.span / r.step), end: e, live: end == null || e >= Math.ceil(now / r.step) * r.step };
}

// The history for one window (app: an app id, or null for the machine). Re-read every minute while
// the window reaches the present; an older window is read once.
export function useHistory(range, end, app = null) {
  const [data, setData] = useState(null);
  const w = windowOf(range, end);
  const key = w ? `${range}|${w.from}|${app}` : '';
  useEffect(() => {
    if (!w) { setData(null); return undefined; }
    const read = () => {
      const x = windowOf(range, end);
      setData(x ? { w: x, h: monitor.history({ from: x.from, step: x.step, n: x.n, app }) } : null);
    };
    read();
    if (!w.live) return undefined;
    const timer = setInterval(read, 60000);
    return () => clearInterval(timer);
  }, [key]);
  return data;
}

// [rx, tx] in bytes per second per bucket (the chart's unit) and the window's totals in bytes.
export function historyBuckets(d) {
  if (!d?.h) return { buckets: [], totals: [0, 0] };
  const { rx, tx } = d.h, step = d.w.step;
  let r = 0, t = 0;
  const buckets = rx.map((v, i) => { r += v; t += tx[i]; return [v / step, tx[i] / step]; });
  return { buckets, totals: [r, t] };
}

const when = (s, withDate) => {
  const d = new Date(s * 1000);
  return withDate ? `${formatDate(d, 'short')} ${formatTime(d)}` : formatTime(d);
};

// "Oct 18 14:00" … "15:00" under the chart, and the window as one line for the header.
export function rangeLabels(d, range) {
  if (!d?.w) return ['', ''];
  const longRange = range === '7d' || range === '30d';
  return [when(d.w.from, true), d.w.live ? '' : when(d.w.end, longRange || new Date(d.w.from * 1000).getDate() !== new Date(d.w.end * 1000).getDate())];
}

function Arrow({ glyph, on, onClick }) {
  const t = useTheme();
  return (
    <View onClick={on ? onClick : undefined} style={{ width: 22, height: 18, borderRadius: 5, justifyContent: 'center', alignItems: 'center',
      backgroundColor: t.control, opacity: on ? 1 : 0.4 }}>
      <Text style={{ fontSize: 11, fontWeight: 700, color: t.text }}>{glyph}</Text>
    </View>
  );
}

// The zoom (live minute, 1 h, 24 h, 7 d, 30 d) and, on a history zoom, ‹ › and "Now".
export function HistoryControls({ range, setRange, end, setEnd, oldest }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const w = windowOf(range, end);
  const canBack = !!w && (!oldest || w.from > oldest);
  const step = (dir) => {
    const r = RANGES[range];
    const e = (w?.end ?? Math.floor(Date.now() / 1000)) + dir * r.span;
    setEnd(e >= Math.floor(Date.now() / 1000) ? null : e);
  };
  return (
    <View style={{ gap: 4 }}>
      <Segmented small fill value={range} onChange={(v) => { setRange(v); setEnd(null); }}
        options={[['live', tr('monitor.hist.live')], ['1h', tr('monitor.hist.1h')], ['24h', tr('monitor.hist.24h')], ['7d', tr('monitor.hist.7d')], ['30d', tr('monitor.hist.30d')]]} />
      {w ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, direction: 'ltr' }}>
          <Arrow glyph="‹" on={canBack} onClick={() => step(-1)} />
          <Arrow glyph="›" on={!w.live} onClick={() => step(1)} />
          <View style={{ flex: 1 }} />
          <View onClick={w.live ? undefined : () => setEnd(null)} style={{ paddingHorizontal: 8, height: 18, borderRadius: 5, justifyContent: 'center',
            backgroundColor: w.live ? 'transparent' : t.accent }}>
            <Text style={{ fontSize: 10, fontWeight: 600, color: w.live ? t.textDim : '#fff' }}>{tr('monitor.now')}</Text>
          </View>
        </View>
      ) : null}
    </View>
  );
}

// The chart for a history window (data from useHistory: the machine's or one app's).
export function HistoryChart({ data: d, range, height = 52, compact }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const { buckets, totals } = historyBuckets(d);
  const [left, right] = rangeLabels(d, range);
  if (d && !d.h) return <Text style={{ fontSize: 11, color: t.textDim, lineHeight: 1.35 }}>{tr('monitor.hist.unavailable')}</Text>;
  const empty = d?.h && !d.h.enabled && !d.h.res;
  return (
    <View style={{ gap: 4 }}>
      <TrafficChart buckets={buckets.length ? buckets : [[0, 0], [0, 0]]} shift={1} height={height} totals={compact ? null : totals}
        caption={tr(`monitor.hist.per.${range}`)} axis={compact ? null : [left, right || tr('monitor.now')]} compact={compact} />
      {empty ? <Text style={{ fontSize: 10, color: t.textDim, lineHeight: 1.35 }}>{tr('monitor.hist.off')}</Text> : null}
      {!empty && d?.h && !d.h.res ? <Text style={{ fontSize: 10, color: t.textDim, lineHeight: 1.35 }}>{tr('monitor.hist.none')}</Text> : null}
    </View>
  );
}
