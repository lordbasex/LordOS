// appsoff.js — which built-in apps the person deactivated (settings.json → appsOff; the rules are in
// appsoffmodel.js, the actions in appsoffactions.js). Small on purpose: apps.js, defaults.js and the
// alarms read it without pulling in the Dock, Mail or Backups.
//
//   appsOff()            ['mail', 'photos'] (only deactivatable ids)
//   isAppOff(id)         true for a deactivated built-in app
//   useAppsOff()         the list, re-rendering when it changes
import { settings } from './settings.js';
import { useStore } from './store.js';
import { normalizeOff, isOff } from './appsoffmodel.js';

export const appsOff = (s = settings.get()) => normalizeOff(s.appsOff);
export const isAppOff = (id) => isOff(appsOff(), id);
export function useAppsOff() { return normalizeOff(useStore(settings, (s) => s.appsOff)); }
