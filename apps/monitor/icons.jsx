// icons.jsx — ícono de la app y glifos chicos de los encabezados del Monitor, dibujados con
// View (sin fuentes de íconos) o en píxeles.
import { View, Image } from '@lordos/sdk';
import { canvas, pack, dot } from './draw.js';

// Ícono de la app: fondo oscuro con un gráfico verde de actividad (como el de TangoOS).
const iconCache = new Map();
function iconPixels(s) {
  if (iconCache.has(s)) return iconCache.get(s);
  const cv = canvas(s, s);
  const { px } = cv, r = s * 0.225;
  for (let y = 0; y < s; y++) {
    const g = 0.5 - 0.5 * (y / s);
    const c = pack(Math.round(38 + 30 * g), Math.round(42 + 30 * g), Math.round(52 + 34 * g), 1);
    for (let x = 0; x < s; x++) {
      const dx = Math.max(r - x - 0.5, x + 0.5 - (s - r), 0), dy = Math.max(r - y - 0.5, y + 0.5 - (s - r), 0);
      const d = Math.hypot(dx, dy);
      if (d <= r - 0.5) px[y * s + x] = c;
      else if (d < r + 0.5) { px[y * s + x] = c; cv.data[(y * s + x) * 4 + 3] = Math.round((r + 0.5 - d) * 255); }
    }
  }
  // guías
  for (const gy of [0.38, 0.58, 0.78]) {
    const y = Math.round(s * gy);
    for (let x = Math.round(s * 0.14); x < s * 0.86; x++) px[y * s + x] = pack(255, 255, 255, 0.08);
  }
  // serie: zigzag verde con relleno
  const pts = [[0.14, 0.7], [0.3, 0.62], [0.42, 0.72], [0.55, 0.36], [0.66, 0.5], [0.76, 0.3], [0.86, 0.4]];
  const yAt = (fx) => {
    for (let i = 0; i < pts.length - 1; i++) {
      const [x0, y0] = pts[i], [x1, y1] = pts[i + 1];
      if (fx >= x0 && fx <= x1) return y0 + ((fx - x0) / (x1 - x0)) * (y1 - y0);
    }
    return null;
  };
  for (let x = Math.ceil(s * 0.14); x < s * 0.86; x++) {
    const fy = yAt((x + 0.5) / s);
    if (fy === null) continue;
    for (let y = Math.round(fy * s); y < s * 0.82; y++) px[y * s + x] = pack(48, 209, 88, 0.28 * (1 - (y / s - fy) / (0.82 - fy + 0.001)) + 0.04);
  }
  const lw = Math.max(0.9, s * 0.028);
  for (let i = 0; i < pts.length - 1; i++) {
    const [x0, y0] = pts[i], [x1, y1] = pts[i + 1], n = Math.ceil(Math.hypot(x1 - x0, y1 - y0) * s * 2);
    for (let k = 0; k <= n; k++) dot(cv, (x0 + ((x1 - x0) * k) / n) * s, (y0 + ((y1 - y0) * k) / n) * s, lw, '#30d158');
  }
  iconCache.set(s, cv);
  return cv;
}

export function MonitorIcon({ size: s = 32 }) {
  const n = Math.max(8, Math.round(s));
  return <Image pixels={iconPixels(n)} style={{ width: s, height: s, boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, borderRadius: s * 0.225 }} />;
}

// ---- glifos de 13 px ----

export function Glyph({ name, color, size = 13 }) {
  const s = size, b = Math.max(1.2, s * 0.11);
  const box = { width: s, height: s, alignItems: 'center', justifyContent: 'center' };
  switch (name) {
    case 'cpu':
      return (
        <View style={box}>
          <View style={{ width: s * 0.8, height: s * 0.8, borderRadius: 2, border: `${b}px solid ${color}`, alignItems: 'center', justifyContent: 'center' }}>
            <View style={{ width: s * 0.32, height: s * 0.32, borderRadius: 1, backgroundColor: color }} />
          </View>
        </View>
      );
    case 'memory':
      return (
        <View style={box}>
          <View style={{ width: s, height: s * 0.55, borderRadius: 2, border: `${b}px solid ${color}`, flexDirection: 'row', gap: s * 0.12, alignItems: 'center', justifyContent: 'center' }}>
            {[0, 1, 2].map((i) => <View key={i} style={{ width: s * 0.1, height: s * 0.2, backgroundColor: color }} />)}
          </View>
        </View>
      );
    case 'network':
      return (
        <View style={box}>
          <View style={{ position: 'absolute', left: s * 0.36, top: 0, width: s * 0.3, height: s * 0.3, borderRadius: 1, border: `${b}px solid ${color}` }} />
          <View style={{ position: 'absolute', left: s * 0.49, top: s * 0.3, width: b, height: s * 0.25, backgroundColor: color }} />
          <View style={{ position: 'absolute', left: s * 0.15, top: s * 0.52, width: s * 0.7, height: b, backgroundColor: color }} />
          <View style={{ position: 'absolute', left: 0, top: s * 0.68, width: s * 0.3, height: s * 0.3, borderRadius: 1, border: `${b}px solid ${color}` }} />
          <View style={{ position: 'absolute', right: 0, top: s * 0.68, width: s * 0.3, height: s * 0.3, borderRadius: 1, border: `${b}px solid ${color}` }} />
        </View>
      );
    case 'disk':
      return (
        <View style={box}>
          <View style={{ width: s, height: s * 0.62, borderRadius: 3, border: `${b}px solid ${color}`, justifyContent: 'flex-end', padding: 1.5 }}>
            <View style={{ width: s * 0.18, height: s * 0.14, borderRadius: 1, backgroundColor: color, alignSelf: 'flex-end' }} />
          </View>
        </View>
      );
    case 'temp':
      return (
        <View style={box}>
          <View style={{ width: s * 0.34, height: s * 0.66, borderRadius: s * 0.17, border: `${b}px solid ${color}` }} />
          <View style={{ width: s * 0.46, height: s * 0.46, borderRadius: s * 0.23, backgroundColor: color, marginTop: -s * 0.2 }} />
        </View>
      );
    case 'fan':
      return (
        <View style={box}>
          <View style={{ width: s * 0.92, height: s * 0.92, borderRadius: s * 0.46, border: `${b}px solid ${color}`, alignItems: 'center', justifyContent: 'center' }}>
            <View style={{ width: s * 0.28, height: s * 0.28, borderRadius: s * 0.14, backgroundColor: color }} />
          </View>
        </View>
      );
    case 'activity':
      return (
        <View style={{ ...box, flexDirection: 'row', alignItems: 'flex-end', gap: s * 0.1 }}>
          {[0.4, 0.9, 0.55, 0.75].map((h, i) => <View key={i} style={{ width: s * 0.15, height: s * h, borderRadius: 1, backgroundColor: color }} />)}
        </View>
      );
    case 'globe':
      return (
        <View style={box}>
          <View style={{ width: s * 0.92, height: s * 0.92, borderRadius: s * 0.46, border: `${b}px solid ${color}`, alignItems: 'center', justifyContent: 'center' }}>
            <View style={{ width: s * 0.36, height: s * 0.92, borderRadius: s * 0.18, border: `${b}px solid ${color}` }} />
          </View>
          <View style={{ position: 'absolute', left: s * 0.06, right: s * 0.06, top: s * 0.48, height: b, backgroundColor: color }} />
        </View>
      );
    // (0.8.5) LordOS's own components and the system services (Monitor → Network rows)
    case 'printer':
      return (
        <View style={box}>
          <View style={{ width: s * 0.56, height: s * 0.26, borderTopLeftRadius: 1, borderTopRightRadius: 1, border: `${b}px solid ${color}`, borderBottomWidth: 0 }} />
          <View style={{ width: s * 0.92, height: s * 0.38, borderRadius: 2, backgroundColor: color }} />
          <View style={{ width: s * 0.56, height: s * 0.24, border: `${b}px solid ${color}`, borderTopWidth: 0 }} />
        </View>
      );
    case 'shield':
      return (
        <View style={box}>
          <View style={{ width: s * 0.72, height: s * 0.86, borderTopLeftRadius: s * 0.12, borderTopRightRadius: s * 0.12, borderBottomLeftRadius: s * 0.36,
            borderBottomRightRadius: s * 0.36, border: `${b}px solid ${color}`, alignItems: 'center', justifyContent: 'center' }}>
            <View style={{ width: s * 0.3, height: b * 1.2, backgroundColor: color }} />
          </View>
        </View>
      );
    case 'tunnel':
      return (
        <View style={box}>
          <View style={{ width: s * 0.86, height: s * 0.62, borderTopLeftRadius: s * 0.43, borderTopRightRadius: s * 0.43, border: `${b}px solid ${color}`, borderBottomWidth: 0,
            alignItems: 'center', justifyContent: 'flex-end' }}>
            <View style={{ width: s * 0.4, height: s * 0.36, borderTopLeftRadius: s * 0.2, borderTopRightRadius: s * 0.2, backgroundColor: color }} />
          </View>
          <View style={{ width: s, height: b, backgroundColor: color }} />
        </View>
      );
    case 'dns':
      return (
        <View style={box}>
          {[0, 1, 2].map((i) => (
            <View key={i} style={{ width: s * 0.86, height: s * 0.24, marginTop: i ? s * 0.06 : 0, borderRadius: 1.5, border: `${b}px solid ${color}`, justifyContent: 'center' }}>
              <View style={{ width: s * 0.1, height: s * 0.1, marginLeft: s * 0.08, borderRadius: s * 0.05, backgroundColor: color }} />
            </View>
          ))}
        </View>
      );
    case 'clock':
      return (
        <View style={box}>
          <View style={{ width: s * 0.92, height: s * 0.92, borderRadius: s * 0.46, border: `${b}px solid ${color}` }} />
          <View style={{ position: 'absolute', left: s * 0.47, top: s * 0.2, width: b, height: s * 0.32, backgroundColor: color }} />
          <View style={{ position: 'absolute', left: s * 0.47, top: s * 0.5, width: s * 0.24, height: b, backgroundColor: color }} />
        </View>
      );
    default:
      return <View style={box} />;
  }
}
