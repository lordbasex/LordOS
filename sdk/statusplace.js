// statusplace.js — where a status item's panel opens, with no screen (statusitems.js, statusPanelAt). The bar can be
// at the top (Flow's menu bar) or at the bottom (the Classic taskbar's default), so the panel goes toward the
// middle of the display on both axes. Tested by server/tests/statusplace_test.mjs.
//
// `box` is the item's [x, y, w, h], `d` its display ({ x, y, w, h }). The result is the style of a View inside
// <Overlay anchor={item}>, whose box is the item's own.
export function panelPlacement(box, d, gap = 4) {
  const [x, y, w, h] = box;
  const below = y + h / 2 < d.y + d.h / 2;
  const fromStart = x + w / 2 < d.x + d.w / 2;
  return { position: 'absolute', [below ? 'top' : 'bottom']: h + gap, [fromStart ? 'left' : 'right']: 0 };
}

// The height a panel placed by panelPlacement may take before it leaves its display (#145): from the gap after the
// item to the far edge of the display's work area (`area`, which leaves out the Dock or a Classic taskbar on that
// side; the display itself when not given), less `margin`. A panel taller than its content keeps its own height; one
// with more rows scrolls them inside this height and keeps its buttons in sight. Never below `least`, so a tiny
// display still shows the panel's head and buttons.
export function panelRoom(box, d, area, gap = 4, margin = 8, least = 160) {
  const [, y, , h] = box;
  const below = y + h / 2 < d.y + d.h / 2;
  const a = area && area.h > 0 ? area : d;
  let room;
  if (below) room = Math.min(d.y + d.h, a.y + a.h) - (y + h + gap) - margin;
  else room = y - gap - Math.max(d.y, a.y) - margin;
  return Math.max(least, Math.floor(room));
}
