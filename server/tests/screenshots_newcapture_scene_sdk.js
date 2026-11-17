// screenshots_newcapture_scene_sdk.js — '@lordos/sdk' for tests/screenshots_newcapture_scene.mjs: only what the
// Screenshots markup editor (apps/screenshots/editor.jsx, ocr.js) imports, over textfield_menu_test.mjs's stub lord.
// The native calls the SDK makes while loading are battery_settings_scene's stubs.
import './battery_settings_scene_stubs.js';
export { View, Text, MediaView, addListener } from '@lordos/runtime';
export { Button, Toggle, Segmented, TextInput, useAppWindow, useKeyboard } from '../../sdk/components.jsx';
export { Toolbar, ToolbarButton, ToolbarGroup, ToolbarDivider, ToolbarSpacer, Hairline } from '../../sdk/controls.jsx';
export { useTheme } from '../../sdk/theme.js';
export { useStore } from '../../sdk/store.js';
export { windows, wm } from '../../sdk/windows.js';
export { t } from '../../sdk/i18n.js';
export { media } from '../../sdk/media.js';
export * as fs from '../../sdk/fs.js';
export { errorText } from '../../sdk/errors.js';
export * as imageEdit from '../../sdk/imageeditmodel.js';
export { settings } from '../../sdk/settings.js';
export { useNarrow } from '../../sdk/narrow.jsx';
export const clipboard = { get: () => lord.clipboard.get(), set: (s) => lord.clipboard.set(String(s)) };
