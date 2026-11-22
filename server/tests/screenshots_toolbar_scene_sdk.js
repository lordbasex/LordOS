// screenshots_toolbar_scene_sdk.js — '@lordos/sdk' for tests/screenshots_toolbar_scene.mjs: only what the Screenshots
// toolbar (apps/screenshots/toolbar.jsx, state.js) imports, over textfield_menu_test.mjs's stub lord. Two microphones
// stand for the sound state.
import './battery_settings_scene_stubs.js';
export { View, Text, addListener, displays as screenDisplays, displayAt } from '@lordos/runtime';
export { ScrollView } from '../../sdk/components.jsx';
export { useAppWindow } from '../../sdk/components.jsx';
export { Tooltip, Popover, DisclosureGlyph } from '../../sdk/controls.jsx';
export { useTheme } from '../../sdk/theme.js';
export { createStore, useStore } from '../../sdk/store.js';
export { settings, useSettings } from '../../sdk/settings.js';
export { windows, wm, workAreaOf } from '../../sdk/windows.js';
export { t, direction } from '../../sdk/i18n.js';
export { placePlayer as placeFloatingBar, rememberPlace as rememberFloatingBar } from '../../sdk/voiceplayermodel.js';
const SOUND = { sources: [
  { name: 'alsa_input.builtin', description: 'Built-in Microphone', monitorOf: -1, default: true },
  { name: 'alsa_input.usb', description: 'USB Headset', monitorOf: -1 },
  { name: 'alsa_output.monitor', description: 'Monitor', monitorOf: 1 },
] };
export const useSound = () => SOUND;
export const microphones = (s) => s.sources.filter((x) => x.monitorOf === -1);
