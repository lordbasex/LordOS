// dnd.jsx — a Files window's drag and drop, outside its main component (GitHub #218). Each pointer move of a drag set
// the window's own state (where the ghost is, which place it is over), so the whole window drew again: in a folder of
// thousands of files that was every card and row, many times a second, and the drag stuttered. Now the move goes to
// two small stores: the ghost follows the pointer by itself, and a place's highlight (a card, a row, a sidebar item,
// a tab) draws again only when the place under the pointer changes, and only for the place it leaves and the one it
// enters. tests/files_drag_scene.mjs counts it.
import { View, Text, createStore, useStore, Layer, shellLayers } from '@lordos/sdk';
import { FolderGlyph, DocGlyph } from './glyphs.jsx';
import { SideItem } from '../../sdk/fileparts.jsx';

// ghost: what is dragged and where, { paths, x, y, alt, favOnly, takes, dir } (screen coordinates), or null.
// target: the drop place under the pointer ('trash:', 'fav:' or a folder's path), or null.
export const createDnd = () => ({ ghost: createStore(null), target: createStore(null) });
const NONE = createDnd();       // a view drawn without a window's drag (a test scene, the file picker)

// true while the drag is over this place; a component that calls it draws again only when that changes
export const useDropHot = (dnd, path) => useStore((dnd || NONE).target, (t) => path != null && t === path);

// a part of a view whose look depends on being the drop place: children(hot) gives its element
export function DropHot({ dnd, path, children }) {
  return children(useDropHot(dnd, path));
}

// a sidebar place that takes drops: lit while the drag is over it
export function DropSideItem({ dnd, ...p }) {
  const hot = useDropHot(dnd, p.drop ? (p.drop.dropTrash ? 'trash:' : p.drop.dropPath) : null);
  return <SideItem {...p} hot={hot} />;
}

// "Add to Favorites", shown in the sidebar only while a folder (or a disk) is dragged
export function FavDropSlot({ dnd, t, label, icon }) {
  const shown = useStore(dnd.ghost, (g) => !!g && !!(g.favOnly || g.dir));
  return shown ? <DropSideItem dnd={dnd} t={t} dim label={label} icon={icon} drop={{ dropPath: 'fav:' }} /> : null;
}

// What is being dragged: in the desktop's drag layer, above every window, so it follows the pointer into another app
// (a window that takes files shows a +); in its own window (root) only when there are no desktop layers.
export function DragGhost({ dnd, root, t }) {
  const g = useStore(dnd.ghost);
  if (!g) return null;
  const layers = shellLayers();
  let { x, y } = g;
  if (!layers) {
    if (!root.current) return null;
    const [lx, ly] = lord.getLayout(root.current.id);
    x -= lx; y -= ly;
  }
  const ghost = (
    <View style={{ position: 'absolute', left: x - 20, top: y - 20, pointerEvents: 'none', opacity: 0.85 }}>
      {g.favOnly || g.dir ? <FolderGlyph size={40} /> : <DocGlyph size={40} kind="other" ext="" dark={t.mode === 'dark'} />}
      {g.paths.length > 1 && (
        <View style={{ position: 'absolute', right: -6, top: -6, minWidth: 18, height: 18, borderRadius: 9, paddingHorizontal: 4, backgroundColor: '#ff3b30', alignItems: 'center', justifyContent: 'center' }}>
          <Text style={{ fontSize: 10, fontWeight: 700, color: '#fff' }}>{g.paths.length}</Text>
        </View>
      )}
      {(g.alt || g.takes) && (
        <View style={{ position: 'absolute', left: 28, top: 26, width: 18, height: 18, borderRadius: 9, backgroundColor: '#30d158', alignItems: 'center', justifyContent: 'center' }}>
          <Text style={{ fontSize: 13, fontWeight: 700, color: '#fff', lineHeight: 13 }}>+</Text>
        </View>
      )}
    </View>
  );
  return layers ? <Layer name="drag">{ghost}</Layer> : ghost;
}
