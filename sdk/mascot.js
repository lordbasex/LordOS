// mascot.js — Lumo, the LordOS guardian (docs/LORDOS-BRAND.md, "Lumo").
//
//   <Lumo pose="confused" size={96} />   a pose: 'wave' (welcome), 'shield' (security), 'run' (busy), 'celebrate'
//                                        (done), 'confused' (error, unavailable, empty), 'sleep' (idle)
//   <Lumo pose="avatar" size={24} />     the head and crown (a tighter crop up to 40 px)
//
// The images are the 3D Lumo: PNG renders in assets/mascot/ made from the 1024 px masters in
// assets/brand/mascot/png/ by tools/gen-mascot.py. lumoSource picks the smallest render that covers the size
// drawn times the largest display scale (a window can move to the sharpest display), so a 112 px empty state
// reads 128 px at 100 % and 256 px at 200 %. Lumo's white fur and violet cape read on light and dark alike;
// never tint, recolor, crop or stretch it. Keep it modest: one Lumo per view, in an empty, error, welcome or
// "done" state (docs/DESIGN.md, "Empty state"). `alt` is the translated description (mascot.<pose>); a caller
// may pass its own.
import { createElement as h } from 'react';
import { Image, displays } from '@lordos/runtime';
import { t } from './i18n.js';

const POSES = new Set(['wave', 'shield', 'celebrate', 'confused', 'sleep', 'run']);
// the renders in assets/mascot/ (tools/gen-mascot.py, UI_SIZES), smallest first
const POSE_PX = [128, 256, 512];
const AVATAR_PX = [128, 256];
const AVATAR_SMALL_PX = [32, 64];
const AVATAR_SMALL_MAX = 40;   // up to this size the avatar is the tighter crop

// the largest display scale (1 = 100 %), at least 1 and at most 3
function maxScale() {
  let s = 1;
  try {
    for (const d of displays() || []) if (d && d.scale > 0) s = Math.max(s, d.scale / 100);
  } catch { /* no displays known yet: 100 % */ }
  return Math.min(s, 3);
}

const pick = (list, px) => list.find((x) => x >= px) || list[list.length - 1];

// pose ('avatar' or one of POSES; anything else is 'confused'), size in logical px, scale (1 = 100 %;
// default the largest display scale) → 'asset:mascot/…png'
export function lumoSource(pose = 'confused', size = 96, scale = maxScale()) {
  const px = Math.ceil(Math.max(1, Number(size) || 96) * Math.max(1, Number(scale) || 1));
  if (pose === 'avatar') {
    return size <= AVATAR_SMALL_MAX ? `asset:mascot/lumo-avatar-small-${pick(AVATAR_SMALL_PX, px)}.png`
      : `asset:mascot/lumo-avatar-${pick(AVATAR_PX, px)}.png`;
  }
  return `asset:mascot/lumo-${POSES.has(pose) ? pose : 'confused'}-${pick(POSE_PX, px)}.png`;
}

export function Lumo({ pose = 'confused', size = 96, alt, style }) {
  return h(Image, {
    src: lumoSource(pose, size),
    alt: alt || t(`mascot.${POSES.has(pose) || pose === 'avatar' ? pose : 'confused'}`),
    style: [{ width: size, height: size }, style],
  });
}
