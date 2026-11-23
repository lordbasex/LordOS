// store_card_scene_sdk.js — '@lordos/sdk' for tests/store_card_scene.mjs: what apps/store/ui.js imports, over
// textfield_menu_test.mjs's stub lord. The card uses only the views, the theme and the scales; the settings store and
// openUrl (the facts row's links) are stand-ins.
export { View, Text, Image } from '@lordos/runtime';
export { spacing, typeScale } from '../../sdk/controls.jsx';
export { useTheme } from '../../sdk/theme.js';
export { useStore } from '../../sdk/store.js';
export const settings = { get: () => ({}), set: () => {}, subscribe: () => () => {} };
export const openUrl = () => {};
