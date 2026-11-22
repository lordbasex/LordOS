// A display's wallpaper (the picture chosen for that display in Settings → Wallpaper, by its connector:
// wallpaperIdFor) and the theme's wallpaper change effect (theme option "Wallpaper change effect"): when the picture
// changes, the old one stays on top for a moment and leaves (fades, opens like a curtain, folds into blinds or
// shutters, or zooms away), uncovering the new one. Only lord.setStyle runs per frame; the effect is skipped with
// reduce motion or "None".
import { Image, View } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { setStyle, tween } from '../cae.js';

const MS = 900;
const BLINDS = 12, SHUTTERS = 8;
const inOut = (x) => (x < 0.5 ? 2 * x * x : 1 - Math.pow(-2 * x + 2, 2) / 2);

function pieces(effect, w, h) {
  if (effect === 'curtain') return [[0, 0, Math.ceil(w / 2), h], [Math.floor(w / 2), 0, w - Math.floor(w / 2), h]];
  if (effect === 'blinds') { const sw = Math.ceil(w / BLINDS); return Array.from({ length: BLINDS }, (_, i) => [i * sw, 0, sw, h]); }
  if (effect === 'shutter') { const sh = Math.ceil(h / SHUTTERS); return Array.from({ length: SHUTTERS }, (_, i) => [0, i * sh, w, sh]); }
  return [[0, 0, w, h]];
}

// The old picture, cut in pieces that each leave their own way: k (0..1) is how far the effect went.
function step(effect, nodes, k, w) {
  nodes.forEach((n, i) => {
    if (effect === 'curtain') setStyle(n, 'translateX', Math.round((i ? 1 : -1) * k * w / 2));
    else if (effect === 'blinds') setStyle(n, 'scaleX', +(1 - k).toFixed(3));
    else if (effect === 'shutter') setStyle(n, 'translateX', Math.round((i % 2 ? 1 : -1) * k * w));
    else if (effect === 'zoom') { setStyle(n, 'scaleX', +(1 + 0.15 * k).toFixed(3)); setStyle(n, 'scaleY', +(1 + 0.15 * k).toFixed(3)); setStyle(n, 'opacity', +(1 - k).toFixed(3)); }
    else setStyle(n, 'opacity', +(1 - k).toFixed(3));
  });
}

// wid: the wallpaper's id (a new size of the same picture plays no effect); src: its file at the display's size
export function Backdrop({ display: d, wid, src, effect, reduce, bg, onContextMenu }) {
  const shown = useRef({ wid, src });            // the picture on screen before this render
  const [old, setOld] = useState(null);          // { src, effect, k } while the effect plays
  const nodes = useRef([]);
  useEffect(() => {
    const was = shown.current;
    shown.current = { wid, src };
    if (was.wid === wid || !was.src || reduce || effect === 'none') return;
    setOld({ src: was.src, effect, k: Date.now() });
  }, [wid, src]);
  useEffect(() => {
    if (!old) return undefined;
    return tween(MS, (k) => step(old.effect, nodes.current.filter(Boolean), k, d.w), () => setOld(null), inOut);
  }, [old]);

  return (
    <View onContextMenu={onContextMenu} style={{ position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h, overflow: 'hidden', backgroundColor: bg }}>
      {src ? <Image src={src} style={{ position: 'absolute', left: 0, top: 0, width: d.w, height: d.h, objectFit: 'cover' }} /> : null}
      {old ? pieces(old.effect, d.w, d.h).map(([x, y, w, h], i) => (
        <View key={`${old.k}:${i}`} ref={(n) => { nodes.current[i] = n; }}
          style={{ position: 'absolute', left: x, top: y, width: w, height: h, overflow: 'hidden', pointerEvents: 'none' }}>
          <Image src={old.src} style={{ position: 'absolute', left: -x, top: -y, width: d.w, height: d.h, objectFit: 'cover' }} />
        </View>
      )) : null}
    </View>
  );
}
