// scrollbar.js — the Text Editor's vertical scroll bar (GitHub #84: a long log could only be scrolled line by line):
// the thumb's size and place for a view of viewH over contentH scrolled to top, and the top for a thumb dragged to pos.
export const BAR = 10;
// the column the bar takes at the right edge; the text is kept out of it (GitHub #109: the end of a long line was drawn
// under the bar and the caret could stop there)
export const BAR_COL = BAR + 4;
// the width the text can use in a view viewW wide: what is left after the line numbers, the padding on both sides
// and, when the bar is shown, its column
export function textWidth(viewW, gutter, pad, barShown) {
  return Math.max(0, viewW - gutter - pad * 2 - (barShown ? BAR_COL : 0));
}
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

// The horizontal bar (GitHub #264: the end of a line wider than the window could only be reached with the caret or
// Shift + wheel). thumbOf and topOfThumb work on either axis; these say how wide the text is and how far it may scroll.
// widest(lines, width): the widest line's width, width(line) giving one line's; RIGHT_ROOM past it, so the caret at the
// end of the widest line is never under the edge
export const RIGHT_ROOM = 40;
export function widest(lines, width) {
  let w = 0;
  for (const line of lines) if (line.length) { const x = width(line); if (x > w) w = x; }
  return w ? w + RIGHT_ROOM : 0;
}
// the most it may scroll sideways for text contentW wide in a column textW wide
export const maxLeft = (contentW, textW) => Math.max(0, contentW - Math.max(0, textW));
export const clampLeft = (left, contentW, textW) => Math.max(0, Math.min(maxLeft(contentW, textW), left));
