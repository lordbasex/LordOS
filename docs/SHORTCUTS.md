# Keyboard shortcuts

The desktop's shortcuts live in one place. A key goes first to `wl_key` (`server/src/wl/wl.c`), which
decides whether a focused Wayland app gets it, then to one dispatcher in the session
(`sdk/shortcuts.js`), which runs before every listener. A key it takes reaches nobody else, so one key
never fires twice.

## The dispatcher

`runtime/events.js` calls the filter set with `setKeyFilter` before any `addListener('keydown')`, and only
`startShell` sets it: an isolated app's process has no dispatcher. The tables:

- `BASE` in `sdk/shortcuts.js`: search, keyboard layout, clipboard, emoji, lock, quit, the media keys, and volume, mute, screen brightness and keyboard light (`sdk/hwkeys.js`), which `keygrab_system_key` keeps from apps. Each
  of those presses shows the new level in a brief indicator (`sdk/levelhud.js`): a rounded translucent panel near the
  bottom of the main display with a drawn icon (speaker or muted speaker, sun, keyboard light) and a bar of 16 segments.
  It stays 1.5 s after the last press, so a held key keeps it up, then fades out in 0.2 s, or just goes with Reduce motion.
  Both themes mount `<LevelHud />` next to `<DesktopHud />`; the login and lock screens don't.
  ❖Esc reads the selection of the window in front aloud and, pressed while reading, stops (`sdk/voice.js`,
  [VOICE.md](VOICE.md)); Settings → AI can turn it off. Linux apps never get a ❖ combination and Esc is a system ❖
  key, so it always reaches the desktop.
- A theme's table, given to `useShortcuts({ grabs, table, onSuperTap, appsPaste })` from its root component
  (`flowTable` in `themes/default/index.jsx`, `classicTable` in `themes/classic/index.jsx`).
- `addShortcuts(entries)` from a module: Screenshots and the Terminal's drop-down window. `runShortcut(id)`
  runs one of those from a table: Lord Classic's ❖⇧S is Screenshots' area capture.

Lookup order is modules, theme, base, and the first entry whose `when(e)` allows it wins. When none does the
key goes on to the listeners, so `when` is how a key stays with an app (⌃Space in the Terminal).

```js
{ id: 'minimize', keys: ['Meta+M'], run: minimizeFront, label: 'shell.shortcuts.minimize', group: 'windows' }
```

`run` is a function, never a command string. `keys` uses `Meta`, `Ctrl`, `Alt`, `Shift` and a key name. Letters
match what the layout produces, or the US position on a layout without Latin letters; other keys match
their position. `label` lists the entry in Settings → Keyboard (`shortcutList()`, read only), and `show`
replaces its keys there; an entry with no `label` is not listed. A held key runs once unless the entry says
`repeat: true`, and its repeats and its release are taken as well.

❖ pressed and released alone calls the theme's `onSuperTap`: Lord Flow's launcher, Lord Classic's start menu.

F10, or the left ⌥ pressed and released alone, opens the menu bar from the keyboard: Lord Flow's bar on the
focused window's display, Lord Classic's menu strip under the focused window's title bar. Each bar registers with
`addMenuBarKeys({ can, run })` and the first whose `can()` says yes opens its first menu with the first row
highlighted; the same key closes it. The Terminal keeps F10 (programs in it use it), the right ⌥ is AltGr and
never counts, and a Linux app has its own menus and gets its keys first. In an open menu the keys follow
`sdk/menunav.js`: ↑/↓ move and wrap, skipping separators and disabled rows, Home/End jump, Enter/Space choose,
→ opens a submenu and ← closes it (mirrored in right-to-left languages), Escape closes one level, letters typed
close together go to the row they start, and → on a row without a submenu or ← in a top menu move to the bar's
next or previous menu.

## The keys kept from apps

`server/src/wl/keygrab.c` holds fixed tables, one per theme, written at build time: keyboard keys only, each
a modifier set plus a letter or an evdev code. `wl_key` compares modifiers exactly, so ⌃Tab and plain ⌃Q
always stay with the app. The shell picks a table by name, `lord.wayland.grabKeys('flow' | 'flow-desktops' | 'classic' | '')`:
any other name is refused, and nothing can add a key. `lord.wayland` does not exist in an isolated app's
process (`js_broker_register`), which has no compositor to change either. `startShell` clears the table,
so a theme that asks for none keeps none.

| Table | Keys |
|---|---|
| `flow` | ❖Tab, ❖⇧Tab (hold), ❖M, ❖H, ❖⌥H, ❖⌥D, ❖,, ❖⌃← ❖⌃→ |
| `flow-desktops` | The same plus ⌃ ← → ↑ ↓: Lord Flow with Settings → Keyboard's Desktops arrows on |
| `classic` | ⌥Tab, ⌥⇧Tab (hold), ⌥F4, ⌃⇧Esc, ❖A, N, D, E, I, R, ❖⇧S, ❖ arrows, ❖1…9, ❖⌃D, ❖⌃F4, ❖⌃← ❖⌃→ |

A Linux app never gets a ❖ combination, so for ❖ the table matters to isolated LordOS apps: without it those
would receive the key and the theme would act as well. `system_meta_key` in `wl.c` still lists the ❖ keys
every theme keeps.

**Hold.** The switcher needs the modifier's release. After a *hold* key, every other key goes to the shell
until the modifier that started it is up, while modifier keys go to both, so an app keeps a correct
state whichever window ends up in front. A key pressed before the switcher opened still gets its release
in the app. With a fullscreen Linux app the switcher does not open and ⌥Tab stays with the app: a
fullscreen surface is presented above every layer.

## The app switcher

`sdk/appswitcher.jsx`, the same in both themes, with its rules in `sdk/switchermodel.js`, mounted by each theme inside its
`ThemeProvider`, in the overlay layer. Apps are grouped as the Dock groups them (`runningApps`), most recently used first. It
opens on the previous app, and the dispatcher gives it every key while open. While the modifier is held: Tab or → and ← move, ⇧Tab goes back, **1 to 9** go straight to an app,
**Z, the key under Esc, or ↓** move through the selected app's windows in a row below it (↑ goes back), Enter or letting go switches, Esc cancels. The keys go by position, so they're the same in every layout.
A Linux or isolated app's window in that row is its real surface, drawn smaller and live; a window the shell draws itself shows its app's icon.
Switching brings the app's top window, on this desktop if it has one. Listing apps from every desktop shows each one's desktop
name, and choosing one elsewhere jumps there. ❖\` and ❖⇧\` in Lord Flow go through the visible windows of the app in front on this
desktop.

Settings → Keyboard (`settings.switcher`): which keys open it (the theme's, ❖Tab, ⌥Tab or both; the grab profile gets `+tab-meta`, `+tab-alt` or `+tab-both`), apps from everywhere, this desktop or this display,
leaving out apps with everything minimized, numbers, the windows row, size, a look (System, Glass, Solid, High contrast, Custom colors) and apps that never show. In Lord Classic, when ❖Tab opens the switcher, the Overview moves to ❖W.

## Stopping a screen share

⌃⌥⇧Esc ends a share of the screen-sharing API ([SCREEN-SHARING.md](SCREEN-SHARING.md)). `on_input` in `server/src/main.c` handles it
in C, before any JavaScript, so it works with the shell hung. Only a physical key gets there: what the API, the remote desktop or the
on-screen keyboard injects takes another path and can't press it. With no share running the key goes on as usual.

## Apps that take the shortcuts (in development)

A VM, a remote desktop or a game can ask for ❖, ⌥Tab and the rest while it's in front: a Linux app with
`zwp_keyboard_shortcuts_inhibit_manager_v1` ([WAYLAND.md](WAYLAND.md), "Keyboard shortcuts inhibit"), Machines' VM
window with `inhibitShortcuts(win, app)` (`sdk/kbinhibit.js`). The person allows it once per app; Settings → Privacy →
Keyboard shortcuts keeps the decision. While it holds and that window has the keyboard, keys go to it before the theme's
table and the dispatcher. Always LordOS's: ❖Esc, which turns it off for that window (a locked pointer and fullscreen
too) and, pressed again, back on; the lock, ❖L and ❖⌃Q; Ctrl+Alt+F1…F12; the media, sound and light keys and Print
Screen; ⌃⌥⌫ and ⌃⌥⇧Esc. Linux apps' global shortcuts (below) don't fire on the keys it gets. A key's release goes where
its press went. The rule is one in C (`kbi_route`,
`server/src/wl/kbinhibit.c`) and its twin in the session (`routeKey`, `sdk/kbinhibitmodel.js`), each with a test.
Not for isolated LordOS apps, which keep the ❖ keys `system_meta_key` lists.

## ❖ as Ctrl in Linux apps (Lord Flow)

With the `flow` table, ❖C, V, X, A, Z, F, T, W, N, L, R, P and S with no other modifier reach the focused Linux
app as Ctrl+key, or Ctrl+Shift+key if its `.desktop` lists the `TerminalEmulator` category (`sdk/wayland.js`
passes that with `lord.wayland.focus`). `send_remapped` follows `wl_send_paste`: only that client, with
Super taken out of the modifiers it sees, and the real press and release, so the app repeats on its own.
`wl_key` returns 2, which the shell receives as `forwarded`: nothing runs, but ❖ no longer counts as pressed
alone. So with a Linux app in front ❖L is that app's, and the screen locks with ❖⌃Q; ❖V pastes and the
clipboard is ❖⇧V. Isolated LordOS apps keep their real ❖ keys.

With `appsPaste` (Lord Flow) ❖V also goes to a LordOS app inside the session, where it pastes, and opens the
clipboard panel only with no window in front. Isolated apps never receive ❖V, so there it stays the panel.

## Desktops

`flowDesktopKeys` and `classicDesktopKeys` in `sdk/desktops.js` are part of each theme's table; [DESKTOPS.md](DESKTOPS.md)
has the keys. In Lord Flow ⌃ arrows are word movement in apps, so they are an option, on by default: off, their
entries stop applying and leave Settings → Keyboard's list (`listed`), and `useShortcuts` asks for the `flow` table
(its `grabs` can change while the theme runs), so the keys reach the apps. ⌃⇧ arrows always stay with the app.

A key pressed with no Linux app in front and released with one (the key that switched to its desktop) has its
release delivered to that app, and the dispatcher never sees it: a new press of a key still marked counts as a press.

## Linux apps' global shortcuts

A call app's push-to-talk or a recorder's hotkeys work while another app is in front through the
`org.freedesktop.portal.GlobalShortcuts` interface (version 1), which lord-server serves itself
(`server/src/ui/portal.c`, the rules in `server/src/ui/gshort.c`; see [FLATPAK.md](FLATPAK.md), "Portals").

- **Asking.** The app opens a session (`CreateSession`) and proposes its shortcuts with `BindShortcuts`: an id, a
  description and a `preferred_trigger` in the spec's text (`CTRL+SHIFT+a`, `LOGO+space`, `ALT+F9`). LordOS's own sheet
  (`sdk/globalshortcuts.jsx`, rows from `sdk/globalshortcutsmodel.js`) lists them under the app's name. Each row starts
  on the proposed key when it is free, and otherwise on none, saying why. Clicking a key field records a new combination
  (Esc stops, Backspace leaves it unassigned). Nothing is bound until the person presses Save, and Cancel answers the app
  "cancelled". While a field records, no app's shortcut fires (`lord.portal.pauseShortcuts`) and no shell shortcut runs
  (`captureKeys` in `sdk/shortcuts.js`).
- **What an app can never get** (`gs_kept`): any ❖ combination, which is the desktop's (the lock screen, search, the
  launcher); the media row and Print Screen (`keygrab_system_key`); Tab and Esc with any modifier (the switcher, menus,
  ending a share); ⌃⌥ with F1…F12, Backspace or Delete; a key with neither ⌃ nor ⌥ unless it is F1…F24, Pause or Scroll
  Lock, so a binding never takes a key used to type or move; whatever the theme's table keeps (`keygrab_match`); and,
  checked by the shell, any combination of `BASE`, a theme's table or a module (`shortcutReserved`, whatever its `when`
  says), such as ⌃Space and F10. A combination one app has is refused for another, and for a second shortcut of the same
  app. lord-server checks the answer again and leaves anything refused unassigned.
- **Delivery.** `js_key` (`server/src/ui/js.c`) asks `portal_shortcut_key` before the input method, the focused Wayland
  app and the shell. A bound key goes to the app that holds it and to nothing else: like GNOME and KDE, the binding
  consumes the key, so the focused app doesn't get it as well. The press sends `Activated` and the release sends
  `Deactivated`, each with the session's handle, the shortcut's id and a time in milliseconds, so push-to-talk works.
  Repeats are taken without a signal, and the release counts even when a modifier was let go first. Signals go only to
  the connection that opened the session and only while that session is bound. A session closed with `Session.Close`, or
  whose connection left the bus, gets nothing more. A key LordOS has since started keeping (another theme) is no longer
  delivered. Letters match what the layout types and other keys their position, as for LordOS's own shortcuts. While a
  window that holds the shortcuts (a VM, a remote desktop; "Apps that take the shortcuts" above) has the keyboard, a press
  it gets (`wl_key_to_inhibitor`) fires no global shortcut; after ❖Esc they work again.
- **A session's states** (`gs_step`): new, asking (one sheet at a time), bound. `BindShortcuts` is accepted once per
  session: when cancelled, the app may ask again. Afterwards changes come from Settings and reach the app as
  `ShortcutsChanged`. `ListShortcuts` answers with the bound ones.
- **Remembered.** The decisions are kept per app, by the app's scope (`desktop:<id>`, `sec/appid.h`), in
  `~/.config/lordos/global-shortcuts.json`, which `lord.fs` can't write (`fsops_protected`). A later session of the
  same app that proposes only shortcuts already decided gets them without a sheet, and an unassigned one stays
  unassigned. An app started outside a scope of its own (from a terminal) is asked every time and nothing of it is kept.
- **Settings → Keyboard** lists every app with its shortcuts below LordOS's own (`apps/settings/GlobalShortcuts.jsx`).
  A key can be changed or cleared there, and Forget removes every decision for the app: its open sessions lose their
  keys at once (`ShortcutsChanged` with none) and its next session asks again.

## Testing

`build/keygrab-test` (`make -C server test`) checks the tables. `build/gshort-test` checks the global shortcuts'
triggers (valid and invalid ones), the combinations LordOS keeps, a session's states and the store;
`tests/global_shortcuts_model_test.mjs` checks the sheet's rows and the combination a key press names, and
`build/portal-gs-test` runs the portal on a private session bus with a second connection as the app (the sheet, press
and release, Settings' changes, a closed session, an app that left the bus). Keys were
tried on a VM with real key events over a Linux app (Mousepad, foot), an app inside the session (Terminal) and an
isolated app (Calendar).
