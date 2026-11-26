// theme-host.js — the theme host (assets/theme-host.bundle.js): LordOS's own code for a community theme's session.
// React, the runtime, the SDK, every native app and the system layer run here, in LordOS's realm, with the full lord;
// lord-server then runs the theme's code in a realm of its own with what prepare() made for it (server/src/ui/js.c
// start_community, sdk/themeapi.js, docs/THEMES-GUIDE.md "Community themes"). The bundle is verified like every bundle
// of LordOS's (tools/code-manifest.mjs, codesig_load).
import * as SDK from '../sdk/index.js';
import { prepareThemeHost, lockdown, runThemeShell } from '../sdk/themeapi.js';

globalThis.__lordThemeHost = Object.freeze({
  prepare: (id, caps) => prepareThemeHost(String(id), Array.isArray(caps) ? caps.map(String) : []),
  lockdown,
  run: () => runThemeShell(),
});

// LordOS's own code in this realm (and the test scenes' drivers, appended to a copy) reach the SDK as in the app host;
// the theme's realm never sees this realm's global object
globalThis.__lordos = Object.freeze({ require: (m) => (m === '@lordos/sdk' ? SDK : null) });
