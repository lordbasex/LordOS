// avatarmodel.js — the square an avatar is cut from (AvatarEditor.jsx), pure (tested by server/tests/avatar_model_test.mjs).
// w, h: the picture's size; zoom: the slider, 0 (the whole short side) to 1 (a quarter of it, at least 64 px);
// center: where the person moved it, in picture pixels. The square never leaves the picture: the center is held in.
export function avatarCrop(w, h, zoom, center) {
  const side = Math.min(w, h);
  if (!(side > 0)) return null;
  const z = Math.max(0, Math.min(1, Number(zoom) || 0));
  const size = Math.max(Math.min(64, side), Math.round(side * (1 - 0.75 * z)));
  const half = size / 2;
  const cx = Math.max(half, Math.min(w - half, Number(center?.x ?? w / 2)));
  const cy = Math.max(half, Math.min(h - half, Number(center?.y ?? h / 2)));
  return { x: Math.round(cx - half), y: Math.round(cy - half), size };
}
