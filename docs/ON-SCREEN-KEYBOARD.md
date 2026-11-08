# On-screen keyboard

A keyboard drawn on the screen for a computer or Raspberry Pi with no physical keyboard: a touchscreen,
a mouse alone, or a remote pointer. It types everywhere a physical keyboard does, including the login
screen and the lock screen, because someone with no keyboard has to be able to log in.

## What it looks like

Large rounded keys in four rows: letters, Shift (tap for one key, tap twice quickly to lock), `?123` for
numbers and symbols with a second page (`1/2`, including Tab, Esc and the arrows), Backspace (repeats
while held), Return, space, and a button that hides it. With more than one layout configured, a key
switches layouts. Colors come from the theme, so it follows light and dark mode and the accent color.

## Opening it

It opens only when someone asks for it; a text field taking focus never opens it, with or without a physical
keyboard (decided by the maintainer on 2026-11-01, replacing the Off, Automatic and Always setting).

The ways to open it: the keyboard button next to the clock (`OskStatus`, in the bar unless Settings → Desktop & Dock →
Menu Bar (Taskbar in Lord Classic) → On-screen keyboard turns it off, issue #58), the shortcut ❖⌃K, and
**Show Keyboard** in Settings → Keyboard. It stays open until it's closed with one of those or its hide button.

The login and lock screens are another user's session (`lordos-greeter`): there it opens only with the keyboard
button next to the language, which is always there, so someone with no physical keyboard can still log in.

### Is a keyboard attached?

`lord.input.hasKeyboard()` answers from the devices `lord-server` has really opened (`input.c`,
`server/src/osk.c`, the fds `lord-sessiond` lends it), and a change arrives as `__lordKeyboards(present)`
with no polling. Neither plugging a keyboard in nor unplugging it opens or closes the panel.

- A keyboard is a device with all the letters, space, Enter, Backspace and Shift. Power buttons, media
  keys and remotes don't count, nor do security tokens that type by themselves (matched by name).
- A desktop PC's PS/2 controller reports a keyboard even with nothing plugged in, so on the `i8042` bus it
  counts only on a laptop (`/sys/class/dmi/id/chassis_type`) or once a letter was pressed on it.
- A remote desktop viewer who typed has a keyboard too, until the session is back on the monitor.

A USB mouse or receiver that declares a full keyboard interface counts as a keyboard, because nothing
tells them apart. Use Always, or the menu bar button, on such a machine.

### Which fields it sees

| Where the field is | How it is known |
|---|---|
| LordOS: `TextInput`, and custom fields that use `ime.attach` (Notes, Terminal, Text Editor) | `sdk/textfocus.js` |
| Isolated apps (`lord-server --app`: Mail, Passwords…) | the `BOP_TEXT_FOCUS` broker hint, obeyed only for the window in front |
| Linux apps that use `zwp_text_input_v3` (GTK, Qt) | `server/src/wl/wl_textinput.c` → `textinput` event |
| Linux apps that don't (X11 apps; Chromium by default) | not known: **the menu bar button opens it** and the keys work the same |

## How a key is typed

Rows are lists of Linux key codes (`sdk/oskmodel.js`). What each key shows comes from the active XKB
layout (`lord.input.keyLabels()`, from scratch states in `keyboard.c`), so Latin American shows `ñ` and
its dead acute, Brazilian `ç`, Dvorak its own order and Arabic its letters. The symbol pages find each
symbol in the layout (`keyFor`) and offer only what it can type, through Shift or AltGr; a symbol that
exists only as a dead key is that key and then space.

`lord.input.tap(code, level)` queues a key; the main loop (`js_input_flush`) presses and releases it
through `js_key`, the same path as a physical key. So layouts, dead keys, compose and input methods
(`ime.c`) work as usual, and from there the key goes to the focused field or `wl_key`, which sends it
only to the client with the keyboard: a Wayland app, an X11 app through Xwayland, or an isolated app.

Keys act on release, as on a tablet: Return can close the field and the keyboard with it, and the finger
must not land on what was underneath. Backspace and space act on press and repeat while held. Pressing
the panel never takes the focus: `keepFocus` on a View (`runtime/events.js`) keeps the press away from
the global `pointerdown` listeners, so a text field stays focused and Search or a menu stays open.

## Keeping the field in view

The panel is in the `input` layer, above everything (`docs/LAYERS.md`), at the bottom of the display the
field is on. `planAvoid` (`sdk/oskmodel.js`) then decides from the caret, or the whole window for a Linux
app that doesn't report one:

1. The field is above the keyboard: nothing moves.
2. A window rises as much as needed, and then shrinks. It goes back when the keyboard closes, unless the
   person moved it meanwhile. `wm.liftForKeyboard`, `wm.dropFromKeyboard`.
3. No window to move (Search, a sheet), or a window that keeps a larger minimum size: the keyboard goes to
   the top of the display.

It decides again after each key, because the caret may have moved to another line, and whenever the field
can have moved with no key: after a commit in this process (`onCommit`, at most once in 80 ms, only with the
panel open) and when an app gives a new place. An isolated app's process tells the session itself after its
own commits and resizes (`sdk/textfocus.js`), only while a field has the focus and only if the place changed.
A place that arrives for the field that already has the focus never reopens a panel the person hid.

A place an app gave is valid for the window size it was given at (`caretOnScreen`). If the window changed size
since, the place is stale and counts as unknown until the app sends a new one, so the keyboard doesn't stay at
the top for a field that has left. Docked at the top it must also clear the field: on a low display, where
the two places overlap, it stays at the bottom. The login screen moves its own column up.

## Right to left

The rows stay left to right in Arabic and Hebrew, as a physical keyboard does; the labels are whatever
the active layout types, and the text goes to the field, which handles its own direction.

## Security and privacy

- **No new way to run anything.** `lord.input.tap` takes one key from a closed list (`osk_key_allowed`):
  letters, digits, punctuation, Tab, Esc, Enter, Backspace, arrows. Never Ctrl, Alt, Super, function or
  system keys; Shift and AltGr come only from a level, and AltGr only if the layout has a third level
  (otherwise Right Alt is plain Alt). A tap can't be a shortcut, and Super, which starts LordOS's
  shortcuts, can't be pressed at all. Modifiers are pressed and released with the key, so none stays down.
- **Same routing as a physical key.** Keys go only to the focused client, as `wl_send_paste` does.
- **Never reachable by isolated apps.** `lord.input` exists only in the theme's and the login screen's
  context (`session_context`); an isolated app has the broker's `lord`. Its one call, `BOP_TEXT_FOCUS`,
  carries a boolean and a position (validated in `brokersrv.c`): it can show the keyboard for the window
  in front and nothing else, and it carries no text.
- **The login screen** keeps its rules: its JS only sends the password to `lord-sessiond`, which verifies it.
- **Typed text is not kept.** The queue holds a key code until the next pass of the main loop and clears
  it; nothing logs a key, there are no suggestions, no history and no learning. `OskStatus` and the panel
  know only which page and Shift state they show.
- **Protected from capture.** The panel is a `protected` node: black in `paint_capture` (screenshots) and
  in the capture canvas (recording and remote desktop). Pressed keys are drawn only inside it.

## Limits

- A fullscreen Linux app is presented above the interface, so the panel can't show over it; leave
  fullscreen first.
- Xwayland doesn't report text fields, and Chromium reports them only with its own IME flag: there, open
  it from the menu bar.
- A Linux window is treated as a whole until the app reports its caret, so it may move more than needed.
