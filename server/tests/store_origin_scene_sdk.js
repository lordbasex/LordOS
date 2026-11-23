// store_origin_scene_sdk.js — '@lordos/sdk' for tests/store_origin_scene.mjs: what apps/store/origincol.js and
// apps/store/ui.js import, over textfield_menu_test.mjs's stub lord. SidebarItem is a View whose texts are its label
// and detail, marked selected with a style the scene reads; t returns the key.
import { createElement as h } from 'react';
import { View, Text, Image } from '@lordos/runtime';
export { View, Text, Image };
export { spacing, typeScale } from '../../sdk/controls.jsx';
export { useTheme } from '../../sdk/theme.js';
export { useStore } from '../../sdk/store.js';
export const settings = { get: () => ({}), set: () => {}, subscribe: () => () => {} };
export const openUrl = () => {};
export const t = (key) => key;
export const ScrollView = ({ style, children }) => h(View, { style }, children);
export const Hairline = () => null;
export const SectionTitle = ({ children }) => h(Text, null, children);
export const SidebarItem = ({ label, detail, selected, onClick }) =>
  h(View, { onClick, style: { opacity: selected ? 0.99 : 1 } }, h(Text, null, label), detail != null ? h(Text, null, String(detail)) : null);
