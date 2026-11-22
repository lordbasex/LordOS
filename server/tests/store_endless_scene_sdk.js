// store_endless_scene_sdk.js — '@lordos/sdk' for tests/store_endless_scene.mjs: only what apps/store/endless.js
// imports, over textfield_menu_test.mjs's stub lord. Button is a View with its label as text.
import { createElement as h } from 'react';
import { View, Text } from '@lordos/runtime';
export { View };
export { spacing } from '../../sdk/controls.jsx';
export const t = (key, vars) => `${key}:${JSON.stringify(vars || {})}`;
export const Button = ({ label, onClick }) => h(View, { onClick }, h(Text, null, label));
