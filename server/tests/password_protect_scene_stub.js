// password_protect_scene_stub.js — imported first by password_protect_scene.mjs: the native modules the system prompts'
// files touch when they load (sound, hardware, vault…) answer an empty list to every call, or are absent (keyboard).
const nothing = new Proxy({}, { get: (o, k) => (typeof k === 'string' ? () => [] : undefined) });
for (const k of ['sound', 'hw', 'vault', 'bt', 'polkit', 'portal', 'web', 'wayland', 'media', 'power', 'net',
  'system', 'session', 'share', 'mouse', 'display', 'perms', 'admin', 'apps', 'fs', 'cast', 'idle', 'flatpak', 'input', 'studio']) lord[k] = nothing;
for (const k of ['keyboard']) lord[k] = undefined;
