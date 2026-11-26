// appskins.jsx — style packs and app skins in the session (the rules are in appskinmodel.js; docs/THEMES-GUIDE.md,
// "Style packs" and "App skins").
//
// A theme calls, at module level (before startShell):
//
//   registerStylePack({ icons: { apps, files, toolbar }, cursors: { fill, outline }, sounds: { startup: [[f, ms], …] },
//                       fonts: { family: 'DejaVu Sans' } });
//   registerAppSkins({ files: { toolbar: MyToolbar, sidebar: MyTaskPane, … } });
//
// An app draws a region through <SkinRegion app="files" region="toolbar" model={vm}>{its own}</SkinRegion>: the theme's
// component gets { model } and is drawn instead; a component that throws is replaced by the app's own for the rest of
// the session (a broken skin never takes an app down). A skin only reads the model and calls its actions: it never
// needs lord.fs or anything else of the app's, and the app checks every action as if its own button had been pressed.
import { createElement, Component } from 'react';
import { Image } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { settings } from './settings.js';
import { cleanSkins, cleanStylePack, skinFor, packIcon, packEffects, packParts, freezeModel, partOn } from './appskinmodel.js';

const skinStore = createStore({});
const packStore = createStore(cleanStylePack(null));

export function registerAppSkins(map) { skinStore.set(() => cleanSkins(map)); }
export function registerStylePack(pack) { packStore.set(() => cleanStylePack(pack)); applyPack(); }

export const useStylePack = () => useStore(packStore);
// The parts this style brings (Settings → Appearance shows a switch for each).
export const useStyleParts = () => { const p = useStore(packStore); const s = useStore(skinStore); return packParts(p, s); };
export const stylePack = () => packStore.get();

// ---- level 1: what lord-server draws or plays ----

let sent = '';
// The pack's pointer colours, sound scheme and font, as the person's switches allow; LordOS's own for a theme without a
// pack (startShell calls this for every theme, so a switch away from one puts the black arrow back).
export function applyPack() {
  if (typeof lord === 'undefined') return;
  const fx = packEffects(packStore.get(), settings.get());
  const key = JSON.stringify(fx);
  if (key === sent) return;
  sent = key;
  try { lord.input?.pointerScheme?.(fx.cursors); } catch (e) { /* older server */ }
  try { for (const [ev, tune] of Object.entries(fx.sounds)) lord.sound?.scheme?.(ev, tune || []); } catch (e) { /* no sound */ }
  try { lord.fonts?.useForTheme?.(fx.font); } catch (e) { /* older server */ }
}
let watching = false;
// Called by startShell: applies the pack and follows the person's switches; plays the theme's startup tune once per
// lord-server (lord-server keeps it from playing again after a theme switch).
export function startStylePack() {
  applyPack();
  if (!watching) { watching = true; settings.subscribe(applyPack); }
  if (packStore.get().sounds.startup && partOn(settings.get(), 'sounds')) {
    // the sound server may come up a moment after the desktop: a few tries
    let n = 0;
    const tryPlay = () => { let ok = false; try { ok = !!lord.sound?.play?.('startup'); } catch (e) { ok = true; } if (!ok && ++n < 6) setTimeout(tryPlay, 1500); };
    setTimeout(tryPlay, 600);
  }
}
// The theme's font for a style: { fontFamily: 'theme' } while it brings one and the person keeps it, else nothing.
export function useThemeFont() {
  const p = useStore(packStore);
  const on = useStore(settings, (s) => partOn(s, 'fonts'));
  return p.fonts && on ? 'theme' : undefined;
}

// An icon of the pack (group 'apps' | 'files' | 'toolbar', by our name), or `fallback` (LordOS's own) when the pack has
// none or the person turned icons off. A component is drawn with { size, color }; an image fills the size.
export function PackIcon({ group, name, size = 16, color, fallback = null }) {
  const p = useStore(packStore);
  const on = useStore(settings, (s) => partOn(s, 'icons'));
  const v = on ? packIcon(p, { themeParts: { icons: true } }, group, name) : null;
  if (!v) return fallback;
  if (typeof v === 'function') return createElement(v, { size, color });
  return createElement(Image, { src: v, style: { width: size, height: size, objectFit: 'contain' } });
}
// Does the pack have that icon now (to choose between a glyph and a drawn icon without rendering)?
export function usePackHas(group, name) {
  const p = useStore(packStore);
  const on = useStore(settings, (s) => partOn(s, 'icons'));
  return !!(on && packIcon(p, { themeParts: { icons: true } }, group, name));
}
// An app icon component wrapped so the pack's icon for that app (when there is one) is drawn instead.
export function packAppIcon(id, Own) {
  if (!Own || Own.__packed) return Own;
  const Wrapped = ({ size, ...rest }) => createElement(PackIcon, { group: 'apps', name: id, size, fallback: createElement(Own, { size, ...rest }) });
  Wrapped.__packed = true;
  return Wrapped;
}

// ---- level 2: app skins ----

export function useSkin(app, region) {
  const skins = useStore(skinStore);
  const layouts = useStore(settings, (s) => partOn(s, 'layouts'));
  return skinFor(skins, { themeParts: { layouts } }, app, region);
}
// Is any region of this app skinned now (an app may lay itself out a little differently then, e.g. no sidebar border)?
export function useSkinned(app) {
  const skins = useStore(skinStore);
  const layouts = useStore(settings, (s) => partOn(s, 'layouts'));
  return layouts && !!skins[app];
}

// A skin that threw: the app's own region for the rest of the session, said once in the log.
const broken = new Set();
class SkinBoundary extends Component {
  constructor(props) { super(props); this.state = { failed: broken.has(props.id) }; }
  static getDerivedStateFromError() { return { failed: true }; }
  componentDidCatch(err) {
    if (!broken.has(this.props.id)) console.warn(`skin ${this.props.id} failed, using the app's own: ${err && err.message}`);
    broken.add(this.props.id);
  }
  render() { return this.state.failed ? this.props.fallback : this.props.children; }
}

// The app's region, or the theme's component for it with the view model. `children` is the app's own drawing.
export function SkinRegion({ app, region, model, children }) {
  const C = useSkin(app, region);
  const id = `${app}.${region}`;
  if (!C || broken.has(id)) return children === undefined ? null : children;
  const own = children === undefined ? null : children;
  return createElement(SkinBoundary, { id, fallback: own }, createElement(C, { model: freezeModel(model) }));
}

