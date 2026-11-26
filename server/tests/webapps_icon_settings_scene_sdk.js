// webapps_icon_settings_scene's '@lordos/sdk': the SDK modules Settings → Web apps (apps/settings/WebApps.jsx, ui.jsx)
// uses, over textfield_menu_test.mjs's stub lord. The app's icons and the window manager are stand-ins: the real ones
// bring the whole desktop (the Dock's apps, windows).
import { createElement as h } from 'react';
import { View } from '@lordos/runtime';
export { View, Text, screen, addListener } from '@lordos/runtime';
export { createStore, useStore } from '../../sdk/store.js';
export { settings, useSettings } from '../../sdk/settings.js';
export { useTheme } from '../../sdk/theme.js';
export { t, useLocale } from '../../sdk/i18n.js';
export { Toggle, Button, ScrollView, Segmented, TextInput, Spinner, useAppWindow } from '../../sdk/components.jsx';
export { GroupBox, ConfirmDialog, spacing, typeScale } from '../../sdk/controls.jsx';
export { menuStep } from '../../sdk/focusmodel.js';
export { Overlay, Layer } from '../../sdk/layers.js';
export { formatBytes } from '../../sdk/system.js';
export * as fs from '../../sdk/fs.js';
export { errorText } from '../../sdk/errors.js';
export * as webAppsModel from '../../sdk/webappsmodel.js';
export {
  useWebApps, webAppsAvailable, webAppCall, launchWebApp, webRunning, refreshSiteIcon, siteIconStatus, webBadges, webDeveloper,
  setWebDeveloperMode, webSearchEngine, setWebSearchEngine, restartWebApp, quitWebApp, restartWebEngine,
} from '../../sdk/webapps.js';
const Icon = ({ size }) => h(View, { style: { width: size, height: size } });
export const webAppIcon = () => Icon;
export const webProfileIcon = () => Icon;
export const wm = { open: () => 0 };
