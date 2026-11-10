// Which confirmation dialog takes the keys, and what a key does in it (ConfirmDialog in
// sdk/controls.jsx; useKeyboard in sdk/components.jsx lets an app's keys go while one is open).
// Pure logic, with no lord.* calls: tests in server/tests/dialog_keys_test.mjs.
//
// As in libadwaita: Escape cancels and Enter does the default response. A destructive dialog's
// default is Cancel, so a stray Enter never empties the Trash or deletes a row; the destructive
// button needs a click, or Tab to it and then Space.

// The button Enter presses: 'cancel' for a destructive dialog, 'ok' otherwise.
export const defaultButton = (destructive) => (destructive ? 'cancel' : 'ok');

const plain = (e) => !e.ctrl && !e.meta && !e.alt;

// What key e does in a dialog whose keyboard focus is on `focus` ('ok' or 'cancel').
// Returns { press: 'ok' | 'cancel' }, { focus: 'ok' | 'cancel' } (Tab moves it), or null (not ours).
export function dialogKey(e, { destructive, focus }) {
  if (e.key === 'Escape') return { press: 'cancel' };
  if (!plain(e)) return null;
  if (e.key === 'Enter') return { press: defaultButton(destructive) };
  // two buttons: Tab and Shift+Tab both go to the other one
  if (e.key === 'Tab') return { focus: focus === 'ok' ? 'cancel' : 'ok' };
  if (e.key === ' ') return { press: focus || defaultButton(destructive) };
  return null;
}

// The open dialogs, oldest first. Each one is { token, win, focused }: win is the id of the window
// it is in, or null for one of the desktop itself (the Dock's «Empty Trash»), which covers every
// window and so is always in front; focused says whether its window is the focused one.
export function createDialogStack() {
  let list = [];
  const subs = new Set();
  const changed = () => { for (const f of subs) f(); };
  const find = (token) => list.find((d) => d.token === token);
  let taken = null;                        // the last key a dialog acted on
  return {
    // a dialog that acted on key e may close before the app's listeners see e: they ask taken(e)
    take(e) { taken = e; },
    taken: (e) => e != null && e === taken,
    open(token, win, focused) { list = [...list, { token, win: win ?? null, focused: !!focused }]; changed(); },
    setFocused(token, focused) {
      const d = find(token);
      if (d && d.focused !== !!focused) { list = list.map((x) => (x === d ? { ...d, focused: !!focused } : x)); changed(); }
    },
    close(token) { if (find(token)) { list = list.filter((d) => d.token !== token); changed(); } },
    owner() { return keyOwner(list); },
    blocks(win, e) { return blocksWindow(list, win) || (e != null && e === taken); },
    // get and subscribe: a store for useStore (sdk/store.js)
    get: () => list,
    subscribe(f) { subs.add(f); return () => subs.delete(f); },
  };
}

// The dialog that takes the keys: the newest one that is in front (the desktop's own, or one in
// the focused window). A dialog in a window behind takes none, and an older one waits for the
// newer to close.
export function keyOwner(list) {
  for (let i = list.length - 1; i >= 0; i--) if (list[i].win === null || list[i].focused) return list[i].token;
  return null;
}

// Whether the keys of the app in window `win` (useKeyboard) wait: a dialog of the desktop is open,
// or one in that window while it is focused.
export function blocksWindow(list, win) {
  return list.some((d) => d.win === null || (win != null && d.win === win && d.focused));
}

// The one stack of this process (the desktop's, or an isolated app's own).
export const dialogStack = createDialogStack();
