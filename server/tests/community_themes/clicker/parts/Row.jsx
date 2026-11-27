// imports its colour from the theme's top folder (a parent import inside the theme, the kit's layout)
import { View, Text } from '@lordos/sdk';
import { FG } from '../tokens.js';

export default function Row({ app }) {
  const Icon = app.Icon;
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, height: 24 }}>
      {Icon ? <Icon size={20} /> : null}
      <Text style={{ fontSize: 12, color: FG }}>{app.name}</Text>
    </View>
  );
}
