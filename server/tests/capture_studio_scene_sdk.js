// capture_studio_scene_sdk.js — '@lordos/sdk' for capture_studio_scene.mjs: the few SDK modules apps/studio needs, not
// the whole desktop.
export { View, Text } from '@lordos/runtime';
export { useTheme } from '../../sdk/theme.js';
export { StudioWindow, studioAvailable } from '../../sdk/studio.jsx';
// (0.8.9.6.2) apps/studio wraps its window in SkinHeader (app skins, sdk/appskins.jsx); with no skin it draws the window as is
export { SkinHeader } from '../../sdk/appskins.jsx';
export const appName = () => '';
export const getApp = () => null;
