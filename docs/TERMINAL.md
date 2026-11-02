# LordOS Terminal

LordOS's native terminal, inspired by popular terminal emulators. It isn't a Linux app running inside LordOS:
the emulation, the PTY and the drawing are LordOS's own C code inside `lord-server`,
and the UI (tabs, panes, profiles, menus) is a React app like the others.

```
apps/terminal/          React: tabs, panes, profiles, search, toolbelt, shortcuts
      │  lord.term.* (server/src/ui/js_term.c)
      ▼
server/src/term/
  term.c        sessions: PTY ↔ emulator, keyboard, mouse, events, Instant Replay, logging,
                tmux -CC (virtual panes), save/restore screen
  vt.c          xterm-256color emulator: VT500 parser, grid + history, reflow, selection,
                search, marks, graphemes, OSC 7/8/52/133/1337, images (OSC 1337 and Sixel)
  pty.c         posix_openpt + fork/exec of the user's shell, without privileges
  hold.c        holders: keep the shells alive if lord-server restarts
  term_paint.c  grid drawing: fonts, ligatures, emoji, boxes, cursor, selection, images
server/src/ui/otf.c     our own OpenType: GSUB substitutions (ligatures, emoji) and CBDT glyphs
server/src/ui/fonts.c   catalog of installed fonts (TTF/OTF/TTC)
apps/terminal/iterm.js  importer for profiles from other terminal emulators (JSON, .itermcolors, XML/binary plist)
apps/terminal/tmux.js   tmux control-mode protocol → native tabs and panes
assets/shell/           integration with bash (lord-bash.sh), zsh (zsh/.zshenv + lord-zsh.zsh)
                        and fish (lord-fish.fish)
assets/fonts/JetBrainsMono-*.ttf  bundled monospace font (OFL)
```

Why C: `cat` of a large file, `htop` or a build produce megabytes per second. Passing them
through JavaScript would be slow on modest machines; here the text goes from the PTY to the
grid and from the grid to the screen without touching QuickJS. Only the *damage* (the rows
that changed) is repainted.

## Screenshots (VM, real lord-server)

| | |
|---|---|
| ![panes](terminal-split-panes.png) | ![schemes](terminal-color-schemes.png) |
| Split panes, `top` on the alternate screen | Preferences → Colors |
| ![transparency](terminal-transparency.png) | ![dropdown](terminal-drop-down.png) |
| Dracula with transparency and blur | Dropdown window (F12), toolbelt, timestamps |
| ![imgcat](terminal-inline-images.png) | ![sixel](terminal-sixel.png) |
| `imgcat` and badge | Sixel |
| ![regex](terminal-regex-search.png) | ![replay](terminal-session-replay.png) |
| Regular-expression search | Instant Replay |
| ![colors](terminal-true-color.png) | ![theme](terminal-theme-switch.png) |
| SGR: bold, italic, underline, strikethrough, direct color | The same shell (PID 4126) is still alive after switching to the Lord Classic theme |
| ![ligatures](terminal-ligatures.png) | ![emoji](terminal-emoji.png) |
| Profile imported from another terminal emulator: Fira Code with ligatures, Dracula, a trigger that highlights "ERROR" | Ligatures, combining marks and color emoji; below, with mode 2027 (👍🏽 🇦🇷 👨‍👩‍👧 as one glyph) |
| ![import](terminal-import-color-scheme.png) | ![keys](terminal-key-bindings.png) |
| Importing a binary preferences plist | Per-profile and global keys, editable shortcuts |
| ![recovered](terminal-session-restored.png) | ![tmux](terminal-tmux.png) |
| After restarting lord-server: same tabs and panes, the counter kept running | `tmux -CC`: tmux windows as tabs, with 3 native panes |
| ![fish](terminal-fish-shell.png) | ![host](terminal-host-profile.png) |
| fish integration (red/blue marks, directory) | Automatic profile switch (rule `~/prod/*` → Production profile, badge PROD) |

## Performance (development VM, 1280×800, CPU)

| Test | Result |
|---|---|
| `seq 1 1000000` (6.9 MB) with transparency + blur | 2.1 s |
| Server RAM with 3 terminals open | ~39 MB |
| Theme load with the Terminal app included | ~40 ms |
| Server RAM with 6 sessions + 3 tmux panes + emoji font | ~43 MB |
| Each session holder (`lord-term-holder`) | ~2.2 MB |

## Parity with popular terminal emulators

Status: ✅ done and tested in the VM · 🟡 partial · ⏳ pending · — doesn't apply to LordOS.

### Emulation

| Feature | Status | Notes |
|---|---|---|
| xterm-256color, direct color (24-bit) | ✅ | Full SGR, `38;2`, `38;5`, subparameters with `:` |
| Bold, faint, italic, underline (single, double, curly), strikethrough, overline, reverse video, blink, hidden | ✅ | |
| Alternate screen, scroll regions, insert/delete, tab stops, DEC graphics set | ✅ | tested with `top`, `less`, `vi` |
| Unicode with wide characters (CJK) | ✅ | glibc's `wcwidth`; a CJK font must be installed (the VM has none: boxes are shown) |
| Combining characters (é, ñ with a separate accent) | ✅ | graphemes in a global table; copied text keeps them |
| Emoji graphemes (👍🏽, flags, 👨‍👩‍👧) | ✅ | with mode 2027 (like contour/wezterm/foot); without it `wcwidth` is respected so the shell doesn't get misaligned |
| Text reflow on resize | ✅ | also with wide characters |
| Mouse modes (1000/1002/1003/1006), focus (1004), bracketed paste (2004), synchronized output (2026) | ✅ | |
| DA, DSR/CPR, DECRQM, XTVERSION, window size replies | ✅ | |
| OSC 8 hyperlinks, OSC 0/2 title, OSC 7 directory, OSC 52 clipboard (write only) | ✅ | reading the clipboard from the shell isn't allowed |
| OSC 9 notifications | ✅ | notice inside the window |
| OSC 4/10/11/12/104 dynamic colors | ✅ | |
| OSC 1337 inline-image protocol (File, `imgcat`) | ✅ | PNG/JPEG/GIF (first frame), with `-w`/`-h` |
| Sixel | ✅ | tested with a sixel image generated with `printf` (see `docs/terminal-sixel.png`) |
| Badge (OSC 1337 SetBadgeFormat), marks, CurrentDir, SetProfile, CursorShape, ReportCellSize, Copy, ClearScrollback, RequestAttention | ✅ | |
| Shell integration (OSC 133): prompt marks, exit code, command run | ✅ | bash, zsh and fish |
| Hand-drawn boxes, blocks and powerline arrows | ✅ | they join without gaps |
| Font choice and "non-ASCII" font | ✅ | installed monospace fonts; synthetic bold if the family has none |
| Ligatures | ✅ | our own GSUB (`calt`/`liga`); same glyphs as HarfBuzz in the tests (JetBrains Mono, Fira Code) |
| Arabic, Hebrew, Indic, Thai (bidi) | ✅ | Visual order (Unicode bidi) and words shaped at their natural width; not on the alternate screen; per-profile option (other terminal emulators have it as experimental). See `LANGUAGES.md` |
| Color emoji | ✅ | CBDT (Noto Color Emoji); not COLR/SVG |
| tmux integration (`tmux -CC`) | ✅ | windows → tabs, panes → native panes; see below |

### Windows, tabs and panes

| Feature | Status | Notes |
|---|---|---|
| Tabs (new, close, move, ❖1…❖9, activity, bell, color, editable title) | ✅ | |
| Split panes (right/down), resize by dragging, navigate, maximize pane, equalize, move to tab | ✅ | |
| Dim inactive panes | ✅ | |
| Multiple windows | ✅ | the SDK's window manager |
| Dropdown window (hotkey window) | ✅ | F12 or ❖\` |
| Transparency and blur | ✅ | since 0.8.5 the window's 1-pixel border and the line under its title bar are drawn opaque in both themes, so nothing behind the window shows through them |
| Status bar | ✅ | directory, process, exit code, size, time |
| Toolbelt | ✅ | jobs, recent commands, captured output, clipboard, directories, profiles, notes |
| Save/restore window arrangement | ✅ | in `settings.json` |
| Undo pane close | ✅ | 5 s, ❖Z |
| Open Quickly (search sessions) | ✅ | ❖⇧O |
| Restore sessions when switching themes | ✅ | the same windows, tabs and panes are reopened |
| Sessions that survive a server restart | ✅ | holders (see below); screen, history and arrangement are recovered |
| Full screen | ✅ | maximizes the window |

### Text, selection and search

| Feature | Status | Notes |
|---|---|---|
| Selection by dragging, double click (word), triple (line), quadruple (smart: URL/path), rectangular (Alt) | ✅ | |
| Copy on select, middle click pastes, Shift extends | ✅ | |
| Keyboard copy mode (vi style) | ✅ | ❖⇧C |
| Search with highlighting, case sensitivity and regular expressions | ✅ | |
| Autocomplete with words on screen | ✅ | ❖; |
| Paste history, command history | ✅ | ❖⇧H, ❖⇧; |
| Paste special (on one line, quoted) | ✅ | |
| Composer (write the command before sending it) | ✅ | ❖⇧. |
| Marks: jump between prompts, add, select the last command's output | ✅ | |
| Per-line timestamps | ✅ | ❖⇧E |
| Ctrl/❖+click on paths and links | 🟡 | paths under `$HOME` open Files; URLs are copied (there is no browser) |
| Instant Replay | ✅ | ❖⌥B, up to 1200 frames / 6 MB per session |
| Log the session to a file, save the contents | ✅ | `~/.local/share/lordos/terminal/`, the documents folder (`XDG_DOCUMENTS_DIR`, in the user's language) |
| Broadcast input to all panes | ✅ | ❖⌥I |
| Alert when the command finishes | ✅ | ❖⌥A |

### Profiles

| Feature | Status | Notes |
|---|---|---|
| Multiple profiles, duplicate, default profile, per-tab profile | ✅ | |
| 16 bundled schemes and editable colors (16 ANSI + text, background, bold, links, cursor, selection) | ✅ | |
| Font size, line spacing, margin, cursor (block/underline/bar, blink, guide) | ✅ | |
| Bright bold, bold/italic font, blinking text, minimum contrast | ✅ | |
| Initial directory (home, previous, other), badge, tab color | ✅ | |
| Configurable history, mouse reporting, wheel on the alternate screen, bell, word characters, Alt as Esc+ | ✅ | |
| Triggers (notice, bell, attention, send text, capture, highlight, mark, title, stop) | ✅ | can be disabled one by one |
| Automatic profile switching by directory and by user@host | ✅ | user@host is reported by the shell integration (OSC 1337 RemoteHost) |
| Configurable keyboard shortcuts | ✅ | Preferences → Keys |
| Per-profile and global keys (send text, Esc+, hex, ignore, action) | ✅ | the profile's win over the global ones and the shortcuts |
| Send text on open, size of new windows in columns × rows | ✅ | |
| Import profiles from other terminal emulators | ✅ | see below |

### Not applicable or pending by design

| Feature of other terminal emulators | Status |
|---|---|
| Password manager, Python API, AI assistant | — no secrets or scripting are exposed to the theme |
| Triggers that run scripts or coprocesses, custom per-profile commands | — LordOS only opens the user's shell; an imported "command" is typed into the shell on open |
| Background image, horizontal spacing | ⏳ |
| COLR/SVG emoji, ligatures with GPOS positioning (more precise marks over letters) | ⏳ |
| Automatic updates, Sparkle | — it's updated with LordOS |

## Shortcuts

❖ = the Super key (the main shortcut modifier). Almost all have a Ctrl+Shift variant.
Super on its own opens the app launcher only if released without having done a shortcut.

| Action | Shortcut |
|---|---|
| New tab / window | ❖T · ❖N |
| Close pane / tab / undo | ❖W · ❖⇧W · ❖Z |
| Split right / down | ❖D · ❖⇧D |
| Next / previous / directional pane | ❖] · ❖[ · ❖⌥arrows |
| Next / previous / numbered tab | ❖⇧] · ❖⇧[ · ❖1…❖9 |
| Find / next / previous | ❖F · ❖G · ❖⇧G |
| Copy / paste / paste history | ❖C · ❖V · ❖⇧H |
| Copy mode / Instant Replay | ❖⇧C · ❖⌥B |
| Bigger / smaller / actual size | ❖= · ❖- · ❖0 |
| Previous / next / add mark | ❖⇧↑ · ❖⇧↓ · ❖⇧M |
| Clear buffer / select all | ❖K · ❖A |
| Preferences / toolbelt / timestamps | ❖, · ❖⇧B · ❖⇧E |
| Dropdown window | F12 |

The full list is in `apps/terminal/keys.js` and in Preferences → Keys, where they can also be
changed (click on the shortcut and press the new combination). Changes are saved in
`settings.json` (`terminal.shortcuts`, `terminal.keyMap`, `perfil.keyMap`).

## Fonts, ligatures and emoji

- Preferences → Text lists the installed **monospace** fonts (`assets/fonts`,
  `/usr/share/fonts`, `/usr/local/share/fonts`, `~/.local/share/fonts`, `~/.fonts`). The
  catalog is built by `server/src/ui/fonts.c`, reading only the `name`, `OS/2`, `post` and
  `hmtx` tables with our own bounds-checking parser; stb_truetype opens only the chosen one.
- Ligatures apply the `calt`/`liga`/`clig` features with `server/src/ui/otf.c`
  (lookups 1, 2, 4, 5, 6 and 7). `make -C server test` builds `build/shape-test` to compare
  against HarfBuzz; with JetBrains Mono and Fira Code the results match.
- Emoji: the first installed CBDT font (on Debian, `fonts-noto-color-emoji`). The PNGs are
  decoded with stb_image and scaled to two cells. Sequences (👍🏽, 🇦🇷, 👨‍👩‍👧) are composed with
  its `ccmp`.
- Fallback for missing characters: profile font → "non-ASCII" font → JetBrains Mono → DejaVu
  Sans Mono → Noto Sans Mono → DejaVu Sans → Noto Sans Symbols → Inter → emoji → empty box.

## Importing profiles from other terminal emulators

Preferences → **Import Profiles…** accepts profiles exported by other terminal emulators
(JSON/plist, `.itermcolors` presets):

| File | What it brings |
|---|---|
| `Profiles.json` (Other Actions → Copy All Profiles as JSON / Save Profile as JSON) or a Dynamic Profile | profiles |
| `*.itermcolors` | color scheme (shows up in Colors → Imported) |
| `~/Library/Preferences/com.googlecode.iterm2.plist` (binary or XML) | all profiles, the default profile, custom schemes and global shortcuts |

Converted: colors (sRGB and Display P3, with light/dark variants), font and size, non-ASCII
font, ligatures, line spacing, cursor, bold/italic, minimum contrast, transparency and blur,
columns × rows, history, mouse reporting, Option as Esc+, bell, confirm on close, initial
directory, badge, tab color, "Bound Hosts" (automatic switching), triggers, profile and global
keys, initial text. What LordOS doesn't have is listed as a warning before importing.
Re-importing a profile with the same GUID updates it instead of duplicating it. Verified in the
VM with real `.itermcolors` files (Dracula, Catppuccin) and with a binary preferences plist.

## Sessions that survive a restart

Each session has a **holder** (`lord-term-holder`, the same binary re-executed with
`--term-holder`): an unprivileged ~2 MB process that only keeps the PTY master open and listens
on a private unix socket (`/run/user/<uid>/lordos-term/`, 0700). If lord-server ends because of
a signal (restart, update) the shell keeps running; on startup, the server connects to each
holder (only from the same user, `SO_PEERCRED`) and receives the PTY via `SCM_RIGHTS`. Before
exiting it saves the screen and history (up to 5000 lines, with colors) in a `.state` file next
to the socket; the app saves the windows → tabs → panes arrangement in `terminal.live`. Logging
out from the theme or Ctrl+Alt+Backspace does close the shells. Instant Replay, images and
history older than 5000 lines are not kept.

## tmux -CC

Running `tmux -CC` (or `tmux -CC attach -t session`) in a tab turns it into the "gateway" of
tmux's control mode: another window opens where each tmux window is a tab and each pane a native
pane (with its history, colors and cursor). In that window:

| Action in LordOS | tmux command |
|---|---|
| ❖T / ❖D / ❖⇧D | `new-window` / `split-window -h` / `split-window -v` |
| ❖W / ❖⇧W | `kill-pane` / `kill-window` |
| Click on a pane, equalize, maximize pane | `select-pane`, `select-layout tiled`, `resize-pane -Z` |
| Resize the window | `refresh-client -C width×height` |
| Esc in the gateway or closing the window | `detach-client` (the tmux session stays alive) |

Each pane's output (`%output`) goes from the C parser straight to its virtual session; what is
typed goes back as `send-keys -H`. If lord-server restarts with tmux attached, the gateway goes
back into control mode and the tmux windows reopen by themselves. Tested with tmux 3.5a in the
VM: attach, typing in a pane, `split-window -v`, `kill-pane`, `new-window`, `refresh-client`,
live output, recovery after a restart and `detach-client`. Equalize and maximize pane are
implemented but not tested on screen.

## Shell integration

The login shell from `/etc/passwd` is used:

| Shell | How it's loaded | Tested |
|---|---|---|
| bash | `bash --rcfile assets/shell/lord-bash.sh -i` (loads `/etc/profile` and `~/.bash_profile`/`~/.profile` first) | ✅ |
| zsh | `ZDOTDIR=assets/shell/zsh`: its `.zshenv` restores the user's `ZDOTDIR`, loads their `.zshenv` and registers `lord-zsh.zsh`; then zsh reads `.zprofile`/`.zshrc` normally | ✅ zsh 5.9 |
| fish | `fish -l --init-command "source …/lord-fish.fish"` (after the user's configuration) | ✅ fish 4.0 |
| others | the login shell, without integration | |

All three emit OSC 133 (prompt, command, output, exit code), OSC 7 (directory) and OSC 1337
RemoteHost (user@host, for automatic profile switching) and define
`imgcat [-w width] [-h height] file…` and `lord_badge text`.

## Security

- `lord.term.create()` is the only LordOS API that starts a process. It only launches the login
  shell of the user already running the session, with no arguments chosen by JS (apart from the
  initial directory).
- The child doesn't inherit capabilities (cleared with `capset`), blocked signals, server
  descriptors or the `SUDO_*` variables.
- OSC 52 can only **write** the clipboard; reading it from a program isn't allowed.
- Session logging and "Save contents" write only inside `$HOME`.
- Holders don't read or write the PTY or run anything; their socket and the `.state` files
  (which contain the screen text) are in a 0700 folder of the user.
- `lord.term.tmuxCommand()` only works while that session is in control mode and sends a single
  line without newlines: it's the same thing the user could type into that tmux.
- `lord.fs.read()` (for the importer) reads regular files under `$HOME`, 8 MB maximum.
- Fonts are only looked up in font folders; those in `~/.fonts` belong to the user.
- (0.8.5) The shell enters `lordos-net-terminal.scope` before exec (`pty.c`), so it and every
  program it starts are the firewall app "Terminal" (`lordos:terminal`): its own row in Monitor →
  Network, with each program's name, and its own rules (`FIREWALL.md`, "LordOS components in their
  own scopes"). Best effort: a terminal that can't be placed still opens.

## Tests

```sh
make -C server test            # builds and runs every suite (tests/run_suites.sh: vt-dump, shape-test, ime-test…)
printf 'hola\033[31m rojo\r\n' | server/build/vt-dump 20 5 12 6 rojo
#                                       columns rows  → resizes to 12×6 and searches for "rojo"
printf 'e\xcc\x81\033[?2027h\xf0\x9f\x87\xa6\xf0\x9f\x87\xb7' | server/build/vt-dump 10 2   # [é][🇦🇷]
server/build/shape-test /usr/share/fonts/truetype/firacode/FiraCode-Regular.ttf liga 'a -> b'
```

In the VM, `tools/lord-type.py` types text with QEMU's virtual keyboard (see
`docs/VM-SETUP.md`, step 8).
