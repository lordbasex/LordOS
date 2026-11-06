// statusplace.js — where a status item's panel opens, with no screen (statusitems.js, statusPanelAt). The bar can be
// at the top (Lord Flow's menu bar) or at the bottom (the Classic taskbar's default), so the panel goes toward the
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
