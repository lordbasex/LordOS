// privacymodel.js — which apps Settings → Privacy lists under a permission, and the choices each row offers (Privacy.jsx).
// Pure; tested by server/tests/privacy_model_test.mjs.
//
//   privacyApps(perm, { packages, systemApps, decisions, listed, nameOf })   [[id, name]] in order: the packages and the
//       built-in (system) apps whose manifest declares the permission, then any app with a saved decision. A built-in app
//       is listed even before it asked (GitHub #219: Screenshots was missing until a decision existed, so a denied capture
//       could not be allowed from here).
//   permChoices(builtinDefault)   ['allow', 'ask', 'deny'], or ['allow', 'deny'] for a permission a built-in app starts
//       with (Screenshots' screen): "ask" would only bring the default back, so the person allows it or doesn't.

export function privacyApps(perm, { packages = [], systemApps = [], decisions = [], listed = () => true, nameOf = (id) => id } = {}) {
  const set = new Map();
  const declares = (a) => Array.isArray(a?.permissions) && a.permissions.includes(perm);
  for (const p of packages) if (declares(p)) set.set(p.id, p.name || p.id);
  for (const a of systemApps) if (a && a.system && !a.package && declares(a) && !set.has(a.id)) set.set(a.id, nameOf(a.id, perm));
  for (const d of decisions) if (d.perm === perm && listed(d)) set.set(d.app, nameOf(d.app, perm));
  return [...set.entries()];
}

export const permChoices = (builtinDefault) => (builtinDefault === 'allow' ? ['allow', 'deny'] : ['allow', 'ask', 'deny']);
