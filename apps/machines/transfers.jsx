// transfers.jsx — the machine window's file copy panel (docs/VIRTUALIZATION.md 6.21): along the bottom of the window,
// every file on its way into the guest (drops) and from it ("Send to LordOS"), each with its name, size, a bar, the
// speed, the time left and Cancel. It hides once all are done. A file from the guest, once kept in the machine's
// quarantine folder, can be shown in Files or dragged out onto a Files window (a folder there, or another app that takes
// files): it is copied there with lord.fs, its marks kept, still never executable. Nothing here opens or runs it.
import { View, Text, addListener, fs, revealPath, openFolder, t as tr, formatBytes } from '@lordos/sdk';
import { useRef, useState } from 'react';
import { usePal, Btn, TONE } from './ui.jsx';
import { midName } from './model.js';
import { xferFigures, xferRunnable } from './xfer.js';

// the language's line for one row's progress, or its outcome
function rowLine(r) {
  if (r.state === 'done') return r.dir === 'out' ? tr('machines.xfer.received') : tr('machines.xfer.sent');
  if (r.state === 'paused') return tr('machines.xfer.paused');
  if (r.state === 'failed') {
    const k = { cancelled: 'cancelled', badHash: 'badHash', noSpace: 'noSpace' }[r.why];
    return k ? tr(`machines.xfer.${k}`) : tr('machines.xfer.failed');
  }
  const f = xferFigures(r, formatBytes);
  let s = tr('machines.xfer.of', { done: f.done, size: f.size });
  if (f.speed) s += ` · ${tr('machines.xfer.speed', { speed: f.speed })}`;
  if (f.left) s += ` · ${f.left.min ? tr('machines.xfer.leftMin', { n: f.left.min }) : tr('machines.xfer.leftS', { n: f.left.s })}`;
  return s;
}

// a kept file from the guest dragged out of the panel: onto a Files folder (its dropPath) it is copied there; onto
// another app that takes files (onFilesDrop) it gets the path. Released anywhere else: nothing
const dropPathOf = (inst) => { for (let n = inst; n; n = n.parent) { if (typeof n.props?.dropPath === 'string' && n.props.dropPath.startsWith('/')) return n.props.dropPath; } return null; };
const filesDropOf = (inst) => { for (let n = inst; n; n = n.parent) if (typeof n.props?.onFilesDrop === 'function') return n.props.onFilesDrop; return null; };
function copyOut(path, dir) {
  const base = path.split('/').pop();
  const dot = base.lastIndexOf('.');
  const stem = dot > 0 ? base.slice(0, dot) : base, ext = dot > 0 ? base.slice(dot) : '';
  for (let n = 1; n < 1000; n++) {
    const to = `${dir}/${n === 1 ? base : `${stem} (${n})${ext}`}`;
    let there = true;
    try { fs.stat(to); } catch (e) { there = false; }
    if (!there) { fs.copy(path, to); return to; }
  }
  return null;
}

export function TransfersPanel({ rows, machine, onCancel, bottom = 0 }) {
  const p = usePal();
  const drag = useRef(null);
  const [ghost, setGhost] = useState(null);       // while a file is dragged out: over a place that takes it, or not
  const [said, setSaid] = useState('');
  if (!rows || !rows.length) return null;
  const startDrag = (r, e) => {
    if (!r.path) return;
    drag.current = { path: r.path, x: e.x, y: e.y, moved: false };
    const mv = addListener('pointermove', (ev) => {
      const d = drag.current;
      if (!d) return;
      if (!d.moved && Math.hypot(ev.x - d.x, ev.y - d.y) < 6) return;
      d.moved = true;
      const ok = !!(dropPathOf(ev.target) || filesDropOf(ev.target));
      setGhost((g) => (g === ok ? g : ok));
    });
    const up = addListener('pointerup', (ev) => {
      mv(); up();
      const d = drag.current;
      drag.current = null;
      setGhost(null);
      if (!d || !d.moved) return;
      const into = dropPathOf(ev.target), other = into ? null : filesDropOf(ev.target);
      try {
        if (into) { const to = copyOut(d.path, into); if (to) setSaid(tr('machines.xfer.copiedTo', { name: to.split('/').pop() })); }
        else if (other) other([d.path], { alt: false, x: ev.x, y: ev.y });
      } catch (err) { setSaid(tr('machines.xfer.copyFailed')); }
    });
  };
  return (
    <View style={{ position: 'absolute', left: 12, right: 12, bottom: bottom + 12, alignItems: 'center' }}>
      <View style={{ width: '100%', maxWidth: 620, borderRadius: 12, backgroundColor: 'rgba(28,28,33,0.95)', paddingVertical: 6 }}
        aria-label={tr('machines.xfer.title')}>
        {rows.slice(-6).map((r, i) => {
          const pct = r.size > 0 ? Math.min(100, Math.floor(r.done * 100 / r.size)) : r.state === 'done' ? 100 : 0;
          const run = r.state === 'running' || r.state === 'paused';
          return (
            <View key={r.key} style={{ paddingHorizontal: 14, paddingVertical: 6, borderTopWidth: i ? 1 : 0, borderColor: 'rgba(255,255,255,0.07)' }}>
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
                <Text style={{ fontSize: 12, color: '#a9a9b3', width: 16, textAlign: 'center' }}>{r.dir === 'out' ? '↓' : '↑'}</Text>
                <View style={{ flex: 1 }} onPress={(e) => startDrag(r, e)}>
                  <Text style={{ fontSize: 13, fontWeight: 600, color: '#f2f2f5', textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>
                    {`${midName(r.name, 44)} · ${tr(r.dir === 'out' ? 'machines.xfer.fromGuest' : 'machines.xfer.toGuest', { machine })}`}
                  </Text>
                  <Text style={{ fontSize: 12, color: r.state === 'failed' ? '#ff9a9a' : '#a9a9b3', textAlign: 'start' }}>
                    {rowLine(r) + (r.state === 'done' && r.dir === 'out' && xferRunnable(r) ? ` · ${tr('machines.xfer.notRun')}` : '')}
                  </Text>
                </View>
                {run ? <Btn label={tr('machines.xfer.cancel')} onClick={() => onCancel(r)} style={{ height: 26 }} /> : null}
                {r.state === 'done' && r.path ? <Btn label={tr('machines.xfer.show')} onClick={() => revealPath(r.path)} style={{ height: 26 }} /> : null}
              </View>
              {run ? (
                <View style={{ marginTop: 5, height: 4, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.16)', direction: 'ltr' }}>
                  <View style={{ width: `${Math.max(2, pct)}%`, height: 4, borderRadius: 2, backgroundColor: r.state === 'paused' ? p.dim : TONE.run }} />
                </View>
              ) : null}
            </View>
          );
        })}
        {rows.some((r) => r.dir === 'out' && r.folder) ? (
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 14, paddingTop: 4 }}>
            <Text style={{ flex: 1, fontSize: 11, color: '#8e8e98', textAlign: 'start' }}>{ghost === true ? tr('machines.xfer.dropHere') : ghost === false ? tr('machines.xfer.dragging') : said || tr('machines.xfer.dragHint')}</Text>
            <Btn label={tr('machines.xfer.openFolder')} onClick={() => openFolder(rows.find((r) => r.folder).folder)} style={{ height: 24 }} />
          </View>
        ) : null}
      </View>
    </View>
  );
}
