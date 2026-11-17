// stylethumb.jsx — the small drawn picture of a desktop style (Settings → Appearance → Desktop style, and its replica in
// the first-start chooser, sdk/firststart.jsx). Drawn with views: our own sketch, no screenshots. Lord Flow ('default')
// has a menu bar and a Dock, Lord Classic a centered taskbar; any other theme is drawn from its theme.json "sketch"
// (sdk/thememeta.js: a rail at the start, a row of pills on top, a framed group of two windows; or a menu bar, a dock or
// a taskbar). Moved here from apps/settings in 0.8.9.
import { View } from '@lordos/runtime';

// The bundled themes drawn by their id.
export const knownStyleTheme = (id) => id === 'default' || id === 'classic';

export function SketchThumb({ sketch, accent, dark }) {
  const glass = dark ? 'rgba(30,30,36,0.85)' : 'rgba(255,255,255,0.82)';
  const win = dark ? '#2c2c30' : '#ffffff';
  const left = sketch.rail ? 16 : 6;
  const top = (sketch.menubar ? 8 : 4) + (sketch.pills ? 10 : 0);
  const bottom = sketch.taskbar ? 13 : sketch.dock ? 14 : 5;
  return (
    <View style={{ width: 132, height: 82, borderRadius: 7, overflow: 'hidden', direction: 'ltr',
      background: dark ? 'linear-gradient(160deg, #2b2d4a, #121218)' : 'linear-gradient(160deg, #cfe0ff, #eef1f7)' }}>
      {sketch.menubar ? <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 6, backgroundColor: glass }} /> : null}
      {sketch.rail ? (
        <View style={{ position: 'absolute', left: 3, top: 3, bottom: 3, width: 10, borderRadius: 3, backgroundColor: glass, alignItems: 'center', paddingTop: 4, gap: 3 }}>
          {[0, 1, 2].map((i) => <View key={i} style={{ width: 5, height: 5, borderRadius: 1.5, backgroundColor: i ? '#8e8e93' : accent }} />)}
        </View>
      ) : null}
      {sketch.pills ? (
        <View style={{ position: 'absolute', left, top: sketch.menubar ? 9 : 4, flexDirection: 'row', gap: 3 }}>
          {[accent, glass, glass].map((c, i) => <View key={i} style={{ width: 16, height: 6, borderRadius: 3, backgroundColor: c }} />)}
        </View>
      ) : null}
      <View style={{ position: 'absolute', left, right: 5, top, bottom, borderRadius: 5, ...(sketch.frame ? { border: `1px solid ${accent}` } : null) }}>
        <View style={{ position: 'absolute', left: 4, top: 4, bottom: 4, width: '58%', borderRadius: 3, backgroundColor: win, boxShadow: '0 1px 3px rgba(0,0,0,0.3)' }}>
          <View style={{ marginLeft: 4, marginTop: 4, height: 3, borderRadius: 2, backgroundColor: accent, width: 22 }} />
        </View>
        <View style={{ position: 'absolute', right: 4, top: 4, bottom: 4, width: '32%', borderRadius: 3, backgroundColor: win, boxShadow: '0 1px 3px rgba(0,0,0,0.3)' }} />
      </View>
      {sketch.dock ? <View style={{ position: 'absolute', left: 40, right: 40, bottom: 3, height: 8, borderRadius: 3, backgroundColor: glass }} /> : null}
      {sketch.taskbar ? <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 10, backgroundColor: glass }} /> : null}
    </View>
  );
}

export function DesktopStyleThumb({ id, accent, dark, sketch }) {
  if (sketch && !knownStyleTheme(id)) return <SketchThumb sketch={sketch} accent={accent} dark={dark} />;
  const glass = dark ? 'rgba(30,30,36,0.85)' : 'rgba(255,255,255,0.82)';
  const win = dark ? '#2c2c30' : '#ffffff';
  const classic = id === 'classic';
  return (
    <View style={{ width: 132, height: 82, borderRadius: 7, overflow: 'hidden', direction: 'ltr',
      background: dark ? 'linear-gradient(160deg, #2b2d4a, #121218)' : 'linear-gradient(160deg, #cfe0ff, #eef1f7)' }}>
      {classic ? null : <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 6, backgroundColor: glass }} />}
      <View style={{ position: 'absolute', left: 22, top: classic ? 10 : 14, width: 86, height: 50, borderRadius: classic ? 3 : 5, backgroundColor: win,
        boxShadow: '0 1px 4px rgba(0,0,0,0.3)' }}>
        <View style={{ flexDirection: 'row', gap: 3, padding: 4, justifyContent: classic ? 'flex-end' : 'flex-start' }}>
          {classic ? [0, 1, 2].map((i) => <View key={i} style={{ width: 5, height: 3, borderRadius: 1, backgroundColor: dark ? '#6a6a72' : '#b8b8c0' }} />)
            : ['#ff5f57', '#febc2e', '#28c840'].map((c) => <View key={c} style={{ width: 4, height: 4, borderRadius: 2, backgroundColor: c }} />)}
        </View>
        <View style={{ marginLeft: 6, height: 4, borderRadius: 2, backgroundColor: accent, width: 30 }} />
      </View>
      {classic ? (
        <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 11, backgroundColor: glass, flexDirection: 'row',
          alignItems: 'center', justifyContent: 'center', gap: 3 }}>
          {[accent, '#8e8e93', '#8e8e93', '#8e8e93', '#8e8e93'].map((c, i) => <View key={i} style={{ width: 6, height: 6, borderRadius: 1.5, backgroundColor: c }} />)}
        </View>
      ) : (
        <View style={{ position: 'absolute', left: 36, right: 36, bottom: 3, height: 9, borderRadius: 3, backgroundColor: glass, flexDirection: 'row',
          alignItems: 'center', justifyContent: 'center', gap: 3 }}>
          {[0, 1, 2, 3].map((i) => <View key={i} style={{ width: 5, height: 5, borderRadius: 1.5, backgroundColor: i ? '#8e8e93' : accent }} />)}
        </View>
      )}
    </View>
  );
}
