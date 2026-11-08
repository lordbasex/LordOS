// placement.js — where Lord Classic's taskbars reserve room and where their panels open (0.8.8.6: a taskbar on every
// display). Plain functions of the displays, so server/tests/classic_taskbar_test.mjs can check them without a
// screen; the start menu's box is startBox in components/StartMenu.jsx.
//
//   workAreaFor(pos, d, every)   a display's work area: without its bar when it has one (sdk/menubarmodel.js hasBar)
//   onBar(pos, x, y, bars)       a point on one of the bars (a click there doesn't close a panel)
//   trayPanel(pos, w, h, d)      quick settings or notifications at the tray's end of display d's bar
import { screen, direction, mainDisplay, hasBar } from '@lordos/sdk';
import { BAR_H, barSize } from './components/Taskbar.jsx';

// Several displays (docs/DISPLAYS.md): a taskbar on every display, each one's work area without its bar, unless
// Settings → Desktop & Dock → Taskbar → "Show the taskbar on every display" is off or the displays are mirrored:
// then the bar is on the main one alone and the others are all work area.
export function workAreaFor(pos, d = mainDisplay(), every = true) {
  const a = { x: d.x, y: d.y, w: d.w, h: d.h };
  if (!hasBar(d, every, screen.mirror)) return a;
  const b = barSize(pos);
  if (pos === 'top') { a.y += b; a.h -= b; } else if (pos === 'left') { a.x += b; a.w -= b; } else if (pos === 'right') a.w -= b; else a.h -= b;
  return a;
}

// Is the point over one of the bars? (a click there doesn't close a panel: its button toggles it, or another bar's
// button moves the panel to that bar's display)
export function onBar(pos, x, y, bars = [mainDisplay()]) {
  const b = barSize(pos);
  return bars.some((m) => {
    const px = x - m.x, py = y - m.y;
    if (px < 0 || py < 0 || px >= m.w || py >= m.h) return false;
    return pos === 'top' ? py < b : pos === 'left' ? px < b : pos === 'right' ? px >= m.w - b : py >= m.h - b;
  });
}

// A panel at the tray's end of the bar on display m: quick settings and notifications. A bottom bar's panel is
// placed by its bottom edge, measured from the bottom of the whole screen (the displays can differ in height).
export function trayPanel(pos, width, height, m = mainDisplay()) {
  const rtl = direction() === 'rtl' && (pos === 'top' || pos === 'bottom');
  const left = pos === 'left' ? m.x + barSize(pos) + 12 : pos === 'right' || !rtl ? m.x + m.w - (pos === 'right' ? barSize(pos) : 0) - 12 - width : m.x + 12;
  if (pos === 'bottom') return { pos: { left, bottom: screen.height - (m.y + m.h - BAR_H - 12) }, top: m.y + m.h - BAR_H - 12 - height, left, height };
  const top = pos === 'top' ? m.y + BAR_H + 12 : m.y + m.h - 12 - height;
  return { pos: { left, top }, top, left, height };
}
