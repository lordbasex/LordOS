// topband.js — the room in Workspaces' top band (tested by server/tests/workspaces_model_test.mjs).
//
// The band holds the activity strip at its start and the status cluster at its end, in one row (components/TopBar.jsx,
// TopBand): the cluster keeps the width of what it shows (the date and the clock are longer in some languages) and the
// strip gets the rest, so the two never overlap in any direction or language. Whether the strip shows the other
// activities by icon only (compact) is decided from the widths its texts need against that room, not from a fixed
// guess per activity.

// the strip's parts (TopBar.jsx Pill, BarButton, BarSeparator): a pill's sides, icon and gap; its widest; the strip's
// padding and the gap between its children; a separator with its margins; a button without a label
export const PILL_PAD = 12, PILL_ICON = 13, PILL_GAP = 7, PILL_MAX = 190;
export const STRIP_PAD = 4, STRIP_GAP = 2, SEPARATOR_W = 9, BUTTON_W = 36, LABEL_PAD = 12, LABEL_GAP = 6, POS_PAD = 8;
// the space kept between the strip and the cluster
export const BAND_GAP = 12;

// The width the strip needs. names: each activity's name width (as measured), active: the index of the one in view,
// newW: the "New" label's width, posW: the position text's ("Activity 2 of 4 · up to 9"), extra: the active activity's
// group chips. compact: the other activities by icon only, no "New" label and no position text.
export function stripWidth({ names, active, newW = 0, posW = 0, extra = 0 }, compact) {
  const pills = names.map((w, i) => {
    const label = compact && i !== active ? 0 : PILL_GAP + w;
    return Math.min(PILL_MAX, 2 * PILL_PAD + PILL_ICON + label);
  });
  const button = compact ? BUTTON_W : LABEL_PAD * 2 + 11 + LABEL_GAP + newW;
  const parts = [...pills, ...(extra ? [extra] : []), SEPARATOR_W, button, SEPARATOR_W, ...(compact ? [] : [posW + 2 * POS_PAD])];
  return 2 * STRIP_PAD + parts.reduce((a, b) => a + b, 0) + STRIP_GAP * (parts.length - 1);
}

// The room the strip has: the band's width less the cluster's (measured; before that, a guess) and the gap.
export const stripRoom = (bandW, clusterW) => Math.max(0, bandW - clusterW - BAND_GAP);

// Does the strip go compact in this room? (2 px of slack for the rounding of measured text.)
export const stripCompact = (room, m) => stripWidth(m, false) > room - 2;
