// windowmenu.js — the Window menu for the window in front, the same in every theme (0.8.5):
//
//   Minimize · Zoom · Fill · Center
//   Move & Resize ›  halves, quarters, thirds, arrangements of two or four windows, previous size
//   Move to <display>  (one item per other display)
//   Move to Desktop ›  the other desktops and a new one
//   Overview
//   Bring All to Front
//   the open windows, the one in front checked
//
// Lord Flow shows it under Window in its menu bar; Lord Classic in each window's Window menu.
import { windows, wm, getApp, windowTitle, onActiveDesktop } from './windows.js';
import { screen, displayFor } from '@lordos/runtime';
import { t } from './i18n.js';
import { moveToDesktopItems, desktopSuffix } from './desktops.js';
import { openOverview } from './overview.jsx';

const own = (w) => !(getApp(w.id)?.builtin && !String(w.id).startsWith('linux#'));   // not the theme's panels

// the visible app windows of this desktop, the front one last (windows.get() is in stacking order)
const stack = () => windows.get().filter((w) => own(w) && !w.minimized && onActiveDesktop(w));

function displayName(d, i) {
  return screen.displays.length > 1 ? t('shell.window.displayN', { n: i + 1, name: d.name || '' }).trim() : d.name;
}

export function moveResizeItems(id) {
  const header = (k) => ({ label: t(`shell.window.section.${k}`), disabled: true });
  const z = (zone, k) => ({ label: t(`shell.window.zone.${k || zone}`), onClick: () => wm.snap(id, zone) });
  const others = stack().filter((w) => w.id !== id).reverse();      // the most recent first
  const two = others.length >= 1, four = others.length >= 2;
  const arrange = (layout) => () => wm.arrange([id, ...others.map((w) => w.id)], layout);
  const w = wm.get(id);
  return [
    header('halves'), z('left'), z('right'), z('top'), z('bottom'),
    '-', header('quarters'), z('top-left'), z('top-right'), z('bottom-left'), z('bottom-right'),
    '-', header('thirds'), z('left-third'), z('center-third'), z('right-third'), z('left-two-thirds'), z('right-two-thirds'),
    '-', header('arrange'),
    { label: t('shell.window.arrange.leftRight'), disabled: !two, onClick: arrange('left-right') },
    { label: t('shell.window.arrange.rightLeft'), disabled: !two, onClick: arrange('right-left') },
    { label: t('shell.window.arrange.topBottom'), disabled: !two, onClick: arrange('top-bottom') },
    { label: t('shell.window.arrange.bottomTop'), disabled: !two, onClick: arrange('bottom-top') },
    { label: t('shell.window.arrange.quarters'), disabled: !four, onClick: arrange('quarters') },
    '-',
    { label: t('shell.window.previousSize'), disabled: !(w && (w.snapped || w.maximized)), onClick: () => wm.snap(id, 'restore') },
  ];
}

export function windowMenuItems(id, { close = true } = {}) {
  const w = id ? wm.get(id) : null;
  const items = [
    { label: t('shell.window.minimize'), hint: '❖M', disabled: !w, onClick: () => wm.minimize(id) },
    { label: t('shell.window.zoom'), disabled: !w, onClick: () => wm.toggleMaximize(id) },
    { label: t('shell.window.fill'), disabled: !w, onClick: () => wm.snap(id, 'max') },
    { label: t('shell.window.center'), disabled: !w, onClick: () => wm.center(id) },
    '-',
    { label: t('shell.window.moveResize'), disabled: !w, submenu: w ? moveResizeItems(id) : [] },
  ];
  if (w && screen.displays.length > 1) {
    const here = displayFor(wm.frame(w));
    items.push('-');
    screen.displays.forEach((d, i) => {
      if (d.id !== here?.id) items.push({ label: t('shell.window.moveTo', { display: displayName(d, i) }), onClick: () => wm.moveToDisplay(id, d.id) });
    });
  }
  items.push('-', { label: t('shell.desktops.moveTo'), disabled: !w, submenu: w ? moveToDesktopItems(id) : [] },
    { label: t('shell.desktops.overview'), onClick: () => openOverview() });
  items.push('-', { label: t('shell.window.bringAll'), onClick: () => { for (const x of stack()) wm.raise(x.id); if (id) wm.raise(id); } });
  if (close) items.push('-', { label: t('shell.window.close'), hint: 'Alt+F4', disabled: !w, onClick: () => wm.close(id) });
  const list = windows.get().filter(own);
  if (list.length) {
    items.push('-');
    for (const x of list) items.push({ label: `${windowTitle(x) || x.app}${desktopSuffix(x)}`, checked: x.id === id, onClick: () => wm.focus(x.id) });
  }
  return items;
}
