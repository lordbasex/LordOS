// notes_editor_scene_sdk.js — '@lordos/sdk' for tests/notes_editor_scene.mjs: only what Notes' editor
// (apps/notes/NoteEditor.jsx) and its right-click menu import, over textfield_menu_test.mjs's stub lord. The clipboard
// is the stub's (sdk/clipboard.js's history is not part of what is tested).
export { View, Text, addListener } from '@lordos/runtime';
export { ime } from '../../sdk/ime.js';
export { takeTextFocus, releaseTextFocus, textFocusOwner } from '../../sdk/components.jsx';
export { ContextMenu } from '../../sdk/contextmenu.jsx';
export const clipboard = { get: () => lord.clipboard.get(), set: (s) => lord.clipboard.set(String(s)) };
