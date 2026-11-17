// The Context Spine: the rail at the start of the main display (or of every display, Settings → Desktop & Dock). From
// the top: the LordOS crown (the system menu: About, Settings, Lock, Log Out, Restart, Shut Down), the activity in view
// (its icon in its colour; a click edits it), the activities used just before it (as many as the display's height has
// room for; a click goes there), Activities (the Overview), Apps (the search panel with every app), Files, and at the
// bottom the shortcuts' help (a lifebuoy, ShortcutsHelp.jsx), the notifications, Settings, the Control Center and the button that collapses the spine to icons (it
// collapses by itself on narrow displays while its setting is Automatic). It reserves its width in the work area
// (model/geometry.js).
// The crown is the brand's full-colour render (assets/icons/lordos-crown.png): never recoloured, no glow or shadow.
// Items answer the pointer with a small spring (motion.js: scale on the icon's cached raster, no React render per frame).
import { View, Text, Image, t as tr, useStore, desktopNames, desktopName } from '@lordos/sdk';
import { memo, useRef, useState } from 'react';
import { Surface } from './Surface.jsx';
import { FocusRing } from './BarButton.jsx';
import { ActivitiesGlyph, AppsGlyph, FolderGlyph, ControlGlyph, Chevron, BellGlyph, GearGlyph, ActivityIcon, LifebuoyGlyph } from '../glyphs.jsx';
import { activities, recentActivities, activityOf } from '../state.js';
import { spineRecentRoom } from '../model/activities.js';
import { tween, EASE } from '../motion.js';

const Item = memo(function Item({ t, label, active, expanded, onClick, glyph, anchor, small, fid, current }) {
  const [hover, setHover] = useState(false);
  const icon = useRef(null);
  const spring = (to, ms = 200) => {
    const n = icon.current?.id;
    if (n) tween(n, { scaleX: [to === 1 ? 1.08 : 1, to], scaleY: [to === 1 ? 1.08 : 1, to] }, { ms, ease: EASE.spring, clear: to === 1 });
  };
  const fg = active ? t.onAccent : hover || current ? t.iconFgHover : t.iconFg;
  return (
    <View ref={anchor} onClick={(e) => onClick(e)} title={expanded ? undefined : label}
      onMouseEnter={() => { setHover(true); spring(1.08, 180); }} onMouseLeave={() => { setHover(false); spring(1, 220); }}
      onMouseDown={() => spring(0.92, 120)} onMouseUp={() => spring(1.08, 160)}
      style={{ alignSelf: 'stretch', marginHorizontal: 6, paddingVertical: small ? 5 : expanded ? 8 : 10, borderRadius: 12, alignItems: 'center', gap: 4,
        backgroundColor: active ? t.accent : hover || current ? t.itemHover : 'transparent' }}>
      <View ref={icon} style={{ width: 22, height: 22, alignItems: 'center', justifyContent: 'center' }}>{glyph(fg, active ? t.accent : t.solidBg)}</View>
      {expanded ? <Text style={{ fontSize: 10.5, fontWeight: 600, color: fg, whiteSpace: 'nowrap', textOverflow: 'ellipsis', maxWidth: 70, textAlign: 'center' }}>{label}</Text> : null}
      {fid ? <FocusRing t={t} fid={fid} /> : null}
    </View>
  );
});

// The activity in view and the recent ones: each one's icon on a tile of its colour. One keyed list, so a switch moves
// two tiles and changes a few styles (size, ring) instead of drawing icons again (docs/WORKSPACES.md, "Performance").
// An activity's icon in the spine: the spine's glyph colour like its other items (the activity's own colour is on its
// pill and its windows), brighter for the one in view.
function ActivityGlyph({ id, color, bg }) {
  const m = activityOf(id);
  return <ActivityIcon icon={m.icon} color={color} bg={bg} size={18} />;
}

const Activities = memo(function Activities({ t, expanded, room, desk, onEditActivity, onActivity }) {
  useStore(activities);
  useStore(desktopNames);
  const recent = useStore(recentActivities).filter((id) => id !== desk).slice(0, room);
  const order = [desk, ...recent];
  return (
    <>
      {order.flatMap((id, i) => [
        <Item key={id} t={t} expanded={expanded} small current={i === 0} fid={i === 0 ? 'current' : `recent:${id}`} label={desktopName(id)} active={false}
          onClick={() => (i === 0 ? onEditActivity(id) : onActivity(id))} glyph={(c, bg) => <ActivityGlyph id={id} color={c} bg={bg} />} />,
        ...(i === 0 && recent.length && expanded ? [<Text key="recent-label" style={{ fontSize: 9, fontWeight: 700, color: t.textDim, marginTop: 4, letterSpacing: 0.4 }}>
          {tr('shell.ws.spine.recent')}</Text>] : []),
      ])}
      <View style={{ alignSelf: 'stretch', height: 1, marginHorizontal: 14, marginVertical: 6, backgroundColor: t.barSeparator }} />
    </>
  );
});

export const Spine = memo(function Spine({ t, rect, mode, rtl, active, desk, unread, onCrown, onActivities, onApps, onFiles, onControl, onToggle, onNotif, onSettings, onHelp,
  onEditActivity, onActivity, controlRef }) {
  const expanded = mode === 'expanded';
  return (
    <Surface t={t} radius={18} style={{ boxShadow: t.spineShadow, position: 'absolute', left: rect.x, top: rect.y, width: rect.w, height: rect.h, paddingVertical: 12, alignItems: 'center', gap: 4 }}>
      <View onClick={onCrown} title={tr('shell.ws.spine.system')} style={{ width: 40, height: 40, borderRadius: 12, alignItems: 'center', justifyContent: 'center', marginBottom: 6,
        backgroundColor: active.menu ? t.hover : 'transparent' }}>
        <Image src="asset:icons/lordos-crown.png" style={{ width: 28, height: 28 }} />
        <FocusRing t={t} fid="crown" />
      </View>
      <Activities t={t} expanded={expanded} room={spineRecentRoom(rect.h)} desk={desk} onEditActivity={onEditActivity} onActivity={onActivity} />
      <Item t={t} expanded={expanded} fid="activities" label={tr('shell.ws.spine.activities')} active={active.overview} onClick={onActivities}
        glyph={(c) => <ActivitiesGlyph color={c} size={20} />} />
      <Item t={t} expanded={expanded} fid="apps" label={tr('shell.ws.spine.apps')} active={active.apps} onClick={onApps}
        glyph={(c) => <AppsGlyph color={c} size={20} />} />
      <Item t={t} expanded={expanded} fid="files" label={tr('shell.ws.spine.files')} active={false} onClick={onFiles}
        glyph={(c) => <FolderGlyph color={c} size={20} />} />
      <View style={{ flex: 1 }} />
      <Item t={t} expanded={expanded} fid="help" label={tr('shell.ws.spine.help')} active={active.help} onClick={onHelp}
        glyph={(c, bg) => <LifebuoyGlyph size={18} color={c} bg={bg} />} />
      <Item t={t} expanded={expanded} fid="notif" label={tr('shell.ws.notif.title')} active={active.notif} onClick={onNotif}
        glyph={(c) => <BellGlyph color={c} size={18} dot={unread ? (active.notif ? t.onAccent : t.accent) : null} />} />
      <Item t={t} expanded={expanded} fid="settings" label={tr('shell.ws.spine.settings')} active={false} onClick={onSettings}
        glyph={(c, bg) => <GearGlyph color={c} bg={bg} size={18} />} />
      <Item t={t} expanded={expanded} fid="control" label={tr('shell.ws.spine.control')} active={active.control} onClick={onControl} anchor={controlRef}
        glyph={(c, bg) => <ControlGlyph color={c} bg={bg} size={20} />} />
      <View onClick={onToggle} title={tr(expanded ? 'shell.ws.spine.collapse' : 'shell.ws.spine.expand')}
        style={{ marginTop: 4, width: 32, height: 24, borderRadius: 8, alignItems: 'center', justifyContent: 'center' }}>
        <Chevron color={t.textDim} size={5} dir={(expanded !== !!rtl) ? 'left' : 'right'} />
      </View>
    </Surface>
  );
});
