// DropHud.jsx — the floating notice of files dropped on the terminal and going up to the server (dropupload.js): each
// file with its bar in orange (uploads, as in Files mode), Cancel while it runs, and Hide. It goes away a few seconds
// after the last one ends; the footer keeps the count while it is hidden.
import { View, Text, useStore, t as tr } from '@lordos/sdk';
import { useEffect } from 'react';
import { transfers, cancel, size } from './sftp.js';
import { drops, hideDrops, clearDrops } from './dropupload.js';

const UP = '#ff9f0a';

// the rows of these sessions: [{ sid, x (the transfer) }], those still shown
export const dropRows = (all, jobs, sids) => sids.flatMap((sid) => (all[sid] && !all[sid].hidden ? all[sid].jobs : [])
  .map((job) => ({ sid, x: jobs.find((j) => j.job === job) })).filter((r) => r.x));

export function DropHud({ t, sids }) {
  const all = useStore(drops);
  const jobs = useStore(transfers);
  const rows = dropRows(all, jobs, sids);
  const running = rows.some((r) => r.x.state === 'run');
  const dir = sids.map((s) => all[s]?.dir).find(Boolean) || '';
  useEffect(() => {
    if (running || !rows.length) return undefined;
    const id = setTimeout(() => { for (const sid of sids) clearDrops(sid, () => false); }, 6000);
    return () => clearTimeout(id);
  }, [running, rows.length]);
  if (!rows.length) return null;
  return (
    <View style={{ position: 'absolute', right: 14, top: 44, width: 300, padding: 10, gap: 8, borderRadius: 10, backgroundColor: t.windowBg,
      border: `1px solid ${t.separator}`, boxShadow: '0 10px 28px rgba(0,0,0,0.4)', direction: 'ltr' }}>
      <View style={{ flexDirection: 'row', alignItems: 'center' }}>
        <Text style={{ flex: 1, fontSize: 12, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr(running ? 'terminal.drop.uploading' : 'terminal.drop.done')}</Text>
        <Text onClick={() => sids.forEach(hideDrops)} style={{ fontSize: 11, color: t.accent }}>{tr('terminal.drop.hide')}</Text>
      </View>
      {dir ? <Text style={{ fontSize: 11, fontFamily: 'monospace', color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{tr('terminal.drop.to', { dir })}</Text> : null}
      {rows.slice(-6).map(({ x }) => {
        const pct = x.total ? Math.min(1, x.done / x.total) : x.state === 'done' ? 1 : 0;
        return (
          <View key={x.job} style={{ gap: 3 }}>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
              <Text style={{ fontSize: 11, color: UP }}>↑</Text>
              <Text style={{ flex: 1, fontSize: 12, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{x.name}</Text>
              <Text style={{ fontSize: 11, color: x.state === 'error' ? '#ff453a' : t.textDim }}>
                {x.state === 'done' ? `✓ ${size(x.total)}` : x.state === 'error' ? tr('terminal.drop.failed') : x.state === 'cancelled' ? tr('terminal.drop.cancelled') : `${Math.round(pct * 100)} %`}
              </Text>
              {x.state === 'run' && <Text onClick={() => cancel(x.job)} style={{ fontSize: 11, color: t.textDim }}>✕</Text>}
            </View>
            <View style={{ height: 3, borderRadius: 2, backgroundColor: t.control }}>
              <View style={{ width: `${Math.round(pct * 100)}%`, height: 3, borderRadius: 2, backgroundColor: x.state === 'error' ? '#ff453a' : UP }} />
            </View>
          </View>
        );
      })}
    </View>
  );
}
