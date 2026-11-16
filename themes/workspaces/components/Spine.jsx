// The Context Spine: the rail at the start of the main display (or of every display, Settings → Desktop & Dock). From
// the top: the LordOS crown (the system menu: About, Settings, Lock, Log Out, Restart, Shut Down), Activities (the
// Overview), Apps (the search panel with every app), Files, and at the bottom the Control Center and the button that
// collapses the spine to icons (it collapses by itself on narrow displays while its setting is Automatic). It reserves
// its width in the work area (model/geometry.js).
// The crown is the brand's full-colour render (assets/icons/lordos-crown.png): never recoloured, no glow or shadow.
// Items answer the pointer with a small spring (motion.js: scale on the icon's cached raster, no React render per frame).
import { View, Text, Image, t as tr } from '@lordos/sdk';
import { memo, useRef, useState } from 'react';
import { Surface } from './Surface.jsx';
import { ActivitiesGlyph, AppsGlyph, FolderGlyph, ControlGlyph, Chevron } from '../glyphs.jsx';
import { tween, EASE } from '../motion.js';

const Item = memo(function Item({ t, label, active, expanded, onClick, glyph, anchor }) {
  const [hover, setHover] = useState(false);
  const icon = useRef(null);
  const spring = (to, ms = 200) => {
    const n = icon.current?.id;
    if (n) tween(n, { scaleX: [to === 1 ? 1.08 : 1, to], scaleY: [to === 1 ? 1.08 : 1, to] }, { ms, ease: EASE.spring, clear: to === 1 });
  };
  const fg = active ? t.onAccent : hover ? t.iconFgHover : t.iconFg;
  return (
    <View ref={anchor} onClick={(e) => onClick(e)} title={expanded ? undefined : label}
      onMouseEnter={() => { setHover(true); spring(1.08, 180); }} onMouseLeave={() => { setHover(false); spring(1, 220); }}
      onMouseDown={() => spring(0.92, 120)} onMouseUp={() => spring(1.08, 160)}
      style={{ alignSelf: 'stretch', marginHorizontal: 6, paddingVertical: expanded ? 8 : 10, borderRadius: 12, alignItems: 'center', gap: 4,
        backgroundColor: active ? t.accent : hover ? t.itemHover : 'transparent' }}>
      <View ref={icon} style={{ width: 22, height: 22, alignItems: 'center', justifyContent: 'center' }}>{glyph(fg, active ? t.accent : t.solidBg)}</View>
      {expanded ? <Text style={{ fontSize: 10.5, fontWeight: 600, color: fg, whiteSpace: 'nowrap', textOverflow: 'ellipsis', maxWidth: 70, textAlign: 'center' }}>{label}</Text> : null}
    </View>
  );
});

export const Spine = memo(function Spine({ t, rect, mode, rtl, active, onCrown, onActivities, onApps, onFiles, onControl, onToggle, controlRef }) {
  const expanded = mode === 'expanded';
  return (
    <Surface t={t} radius={18} style={{ boxShadow: t.spineShadow, position: 'absolute', left: rect.x, top: rect.y, width: rect.w, height: rect.h, paddingVertical: 12, alignItems: 'center', gap: 4 }}>
      <View onClick={onCrown} title={tr('shell.ws.spine.system')} style={{ width: 40, height: 40, borderRadius: 12, alignItems: 'center', justifyContent: 'center', marginBottom: 6,
        backgroundColor: active.menu ? t.hover : 'transparent' }}>
        <Image src="asset:icons/lordos-crown.png" style={{ width: 28, height: 28 }} />
      </View>
      <Item t={t} expanded={expanded} label={tr('shell.ws.spine.activities')} active={active.overview} onClick={onActivities}
        glyph={(c) => <ActivitiesGlyph color={c} size={20} />} />
      <Item t={t} expanded={expanded} label={tr('shell.ws.spine.apps')} active={active.apps} onClick={onApps}
        glyph={(c) => <AppsGlyph color={c} size={20} />} />
      <Item t={t} expanded={expanded} label={tr('shell.ws.spine.files')} active={false} onClick={onFiles}
        glyph={(c) => <FolderGlyph color={c} size={20} />} />
      <View style={{ flex: 1 }} />
      <Item t={t} expanded={expanded} label={tr('shell.ws.spine.control')} active={active.control} onClick={onControl} anchor={controlRef}
        glyph={(c, bg) => <ControlGlyph color={c} bg={bg} size={20} />} />
      <View onClick={onToggle} title={tr(expanded ? 'shell.ws.spine.collapse' : 'shell.ws.spine.expand')}
        style={{ marginTop: 4, width: 32, height: 24, borderRadius: 8, alignItems: 'center', justifyContent: 'center' }}>
        <Chevron color={t.textDim} size={5} dir={(expanded !== !!rtl) ? 'left' : 'right'} />
      </View>
    </Surface>
  );
});
