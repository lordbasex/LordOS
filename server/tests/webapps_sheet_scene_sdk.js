// webapps_sheet_scene_sdk.js — '@lordos/sdk' for tests/webapps_sheet_scene.mjs: only the modules the Store's
// "Add from an address" sheet (apps/store/addweb.js) imports, over textfield_menu_test.mjs's stub lord. The letter
// icon and the notification are stand-ins: the real ones bring the whole desktop (sound, the Dock's apps).
import { createElement as h } from 'react';
import { View } from '@lordos/runtime';
export { View, Text, Image } from '@lordos/runtime';
export const notified = [];
export const notify = (n) => { notified.push(n); };
export const LetterIcon = ({ size }) => h(View, { style: { width: size, height: size } });
export { Button, TextInput } from '../../sdk/components.jsx';
export { spacing, typeScale } from '../../sdk/controls.jsx';
export { useTheme } from '../../sdk/theme.js';
export { probeWebApp, installWebApp, launchWebApp } from '../../sdk/webapps.js';
export * as webAppsModel from '../../sdk/webappsmodel.js';
export { errorText } from '../../sdk/errors.js';
export { t } from '../../sdk/i18n.js';
