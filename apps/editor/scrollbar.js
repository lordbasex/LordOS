// scrollbar.js — the Text Editor's vertical scroll bar (GitHub #84: a long log could only be scrolled line by line):
// the thumb's size and place for a view of viewH over contentH scrolled to top, and the top for a thumb dragged to pos.
export const BAR = 10;
export function thumbOf(viewH, contentH, top) {
  if (contentH <= viewH || viewH <= 0) return null;
  const size = Math.max(24, Math.min(viewH, (viewH * viewH) / contentH));
  const pos = ((viewH - size) * top) / (contentH - viewH);
  return { size, pos: Math.max(0, Math.min(viewH - size, pos)) };
}
export function topOfThumb(viewH, contentH, pos) {
  const th = thumbOf(viewH, contentH, 0);
  if (!th || viewH - th.size <= 0) return 0;
  return Math.max(0, Math.min(contentH - viewH, (pos * (contentH - viewH)) / (viewH - th.size)));
}
