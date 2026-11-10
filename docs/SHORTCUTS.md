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
Screen; ⌃⌥⌫ and ⌃⌥⇧Esc. A key's release goes where its press went. The rule is one in C (`kbi_route`,
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

## Testing

`build/keygrab-test` (`make -C server test`) checks the tables. Keys were tried on a VM with real key events
over a Linux app (Mousepad, foot), an app inside the session (Terminal) and an isolated app (Calendar).
