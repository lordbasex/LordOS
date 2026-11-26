// skins/apps.jsx — Bluebell's skins for the apps whose regions take the shared shapes (a command bar, a list pane;
// docs/THEMES-GUIDE.md "App skins → the shared shapes"): each region is drawn by skins/common.jsx. Apps with their own
// shapes have their own file (files, settings, terminal, editor, store).
import { View, TextInput, useTheme } from '@lordos/sdk';
import { CommandBar, ListPane } from './common.jsx';

// a list pane with the app's search field above it (Notes)
function SearchPane({ model }) {
  const t = useTheme();
  return (
    <View style={{ width: 240, background: t.paneBg }}>
      <View style={{ paddingHorizontal: 10, paddingTop: 10 }}><TextInput value={model.query} onChange={model.actions.search} placeholder={model.title} size={12} /></View>
      <ListPane model={model} width={240} />
    </View>
  );
}

export const appSkins = {
  notes: { sidebar: SearchPane, toolbar: CommandBar },
};
