// LordBench's desktop scene (0.8.6, docs/BENCHMARK.md "Desktop impact"): while a desktop run is in its
// comp.* or desk.* step, Performance opens a second System Information window with this scene and asks
// lord-server to move it along a figure of eight (wm.scriptMove: the drag's own path in C, one step per
// frame, no JS per frame), while one element inside it turns through lord.setStyle (no React render per
// frame). lord-server records its own frame times meanwhile (bench/lbframes.c); nothing here measures.
import { View, Text, useTheme, useLocale, spacing, typeScale, wm, workArea } from '@lordos/sdk';
import { useEffect, useRef } from 'react';

export const SCENE_INSTANCE = 'lordbench-scene';
const SCENE_ID = `sysinfo#${SCENE_INSTANCE}`;
const W = 760, H = 480, TICK_MS = 16;          // the app's minimum size
const PERIOD_MS = 6000;

// the window's place (centered in the work area) and the size of its figure of eight
export function scenePath(a) {
  return {
    x: Math.round(a.x + (a.w - W) / 2), y: Math.round(a.y + (a.h - H) / 2),
    rx: Math.max(0, Math.floor((a.w - W) / 2) - 20), ry: Math.max(0, (a.h - H) - 40),
  };
}

// opens the scene and starts its move once the window is on screen; the returned function stops it
export function startScene() {
  const p = scenePath(workArea());
  wm.open('sysinfo', { instance: SCENE_INSTANCE, args: { scene: true }, frame: { x: p.x, y: p.y, w: W, h: H } });
  let tm = null, tries = 0;
  const go = () => {          // the frame's node exists after the first render
    if (wm.scriptMove(SCENE_ID, { rx: p.rx, ry: p.ry, periodMs: PERIOD_MS }) || ++tries > 50) return;
    tm = setTimeout(go, 50);
  };
  tm = setTimeout(go, 50);
  return () => { clearTimeout(tm); wm.stopScript(); wm.close(SCENE_ID, true); };
}

export function Scene() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const spin = useRef(null);
  useEffect(() => {
    let a = 0, tm = null;
    const step = () => {
      a = (a + 6) % 360;
      const n = spin.current && spin.current.id;
      if (n != null) try { lord.setStyle(n, 'rotate', a); } catch (e) { /* gone */ }
      tm = setTimeout(step, TICK_MS);
    };
    tm = setTimeout(step, TICK_MS);
    return () => clearTimeout(tm);
  }, []);
  const card = (i) => (
    <View key={i} style={{ width: 180, height: 96, borderRadius: 12, padding: spacing.m, gap: 4, backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.05)',
      boxShadow: '0 2px 8px rgba(0,0,0,0.25)' }}>
      <View style={{ width: 28, height: 28, borderRadius: 8, backgroundColor: ['#0a84ff', '#30d158', '#ff9f0a', '#bf5af2', '#ff453a', '#64d2ff'][i % 6] }} />
      <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('sysinfo.perf.sceneTitle')}</Text>
      <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr('sysinfo.perf.sceneCard', { n: i + 1 })}</Text>
    </View>
  );
  return (
    <View style={{ flex: 1, padding: spacing.l, gap: spacing.m, alignItems: 'center', justifyContent: 'center' }}>
      <View ref={spin} style={{ width: 72, height: 72, borderRadius: 16, backgroundColor: t.accent || '#0a84ff', alignItems: 'center', justifyContent: 'center' }}>
        <View style={{ width: 24, height: 24, borderRadius: 12, backgroundColor: '#ffffff' }} />
      </View>
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: spacing.m, justifyContent: 'center' }}>{[0, 1, 2, 3, 4, 5].map(card)}</View>
    </View>
  );
}
