// narrow.jsx — apps in a small window (0.8.8.10): a tile of Workspaces' Main and side or Grid is about 600×470 on a
// 1920×1080 display, so every native app must work down to that size. An app reads its window's width from the window
// store (no polling, no measuring) and, below its breakpoint, hides its sidebar behind a toolbar button; shown again
// there, the sidebar lays over the content instead of squeezing it.
//
//   useWindowWidth()               the app window's width (0 outside a window)
//   useWindowHeight()              its height (0 outside a window): a sheet's scrolling body is capped by it
//   useNarrow(px = NARROW)         true while the window is narrower than px
//   useSidebar(px, key)            { narrow, shown, toggle, close }: the sidebar's state, one choice kept for a wide
//                                  window (shown by default) and one for a narrow one (hidden by default), both kept
//                                  per window (useWindowState)
//   <SidebarToggle shown onClick/> the toolbar button that shows or hides it
//   <SidebarOverlay onClose>       a narrow window's sidebar: over the content, a click beside it closes it
//
// The small sizes every app's manifest keeps: minSize at most [MIN_W, MIN_H].
import { useStore } from './store.js';
import { windows } from './windows.js';
import { useAppWindow, useWindowState } from './components.jsx';
import { View } from '@lordos/runtime';
import { useTheme } from './theme.js';
import { t } from './i18n.js';
import { ToolbarButton } from './controls.jsx';

export const NARROW = 640;
export const MIN_W = 480;
export const MIN_H = 360;

export function useWindowWidth() {
  const { id } = useAppWindow() || {};
  return useStore(windows, (ws) => (id == null ? 0 : ws.find((w) => w.id === id)?.w || 0));
}

export function useWindowHeight() {
  const { id } = useAppWindow() || {};
  return useStore(windows, (ws) => (id == null ? 0 : ws.find((w) => w.id === id)?.h || 0));
}

export function useNarrow(px = NARROW) {
  const w = useWindowWidth();
  return w > 0 && w < px;
}

export function useSidebar(px = NARROW, key = 'sidebar') {
  const narrow = useNarrow(px);
  const [wide, setWide] = useWindowState(`${key}:wide`, true);
  const [small, setSmall] = useWindowState(`${key}:narrow`, false);
  const shown = narrow ? small : wide;
  const toggle = () => (narrow ? setSmall(!small) : setWide(!wide));
  const close = () => (narrow ? setSmall(false) : setWide(false));
  return { narrow, shown, toggle, close };
}

// two panes: a narrow one at the start, drawn (no glyph font needed)
function PanesGlyph({ color, on }) {
  return (
    <View style={{ width: 16, height: 13, borderRadius: 3, border: `1.5px solid ${color}`, flexDirection: 'row', direction: 'ltr', overflow: 'hidden' }}>
      <View style={{ width: 5, backgroundColor: on ? color : 'transparent', borderRightWidth: 1.5, borderRightColor: color }} />
    </View>
  );
}

export function SidebarToggle({ shown, onClick, small }) {
  const th = useTheme();
  return (
    <ToolbarButton small={small} active={shown} title={t(shown ? 'shell.sidebar.hide' : 'shell.sidebar.show')} onClick={onClick}
      icon={<PanesGlyph color={shown ? th.accent : th.text} on={shown} />} />
  );
}

// A narrow window's sidebar: drawn over the content from the start edge, with a shade over the rest that closes it.
export function SidebarOverlay({ width = 240, onClose, children, top = 0 }) {
  const th = useTheme();
  return (
    <View style={{ position: 'absolute', left: 0, right: 0, top, bottom: 0, flexDirection: 'row', zIndex: 50 }}>
      <View style={{ width, alignSelf: 'stretch', backgroundColor: th.sidebarBg || th.windowBg || th.bg, borderEndWidth: 1, borderEndColor: th.separator,
        boxShadow: '0 10px 30px rgba(0,0,0,0.30)' }}>
        {children}
      </View>
      <View onClick={onClose} style={{ flex: 1, backgroundColor: 'rgba(0,0,0,0.18)' }} />
    </View>
  );
}
