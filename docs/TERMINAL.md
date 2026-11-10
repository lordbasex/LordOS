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
| The program's size is the grid's (TIOCSWINSZ) | ✅ | from the pane's logical box only (`term_grid_size`): the same at every display scale, in the overview and while minimizing, none for a 0 × 0 pane; the pty's size is read back at most once a second while output arrives and set again if something else changed it (`term_size_sync`, `tests/term_resize_test.c`) |
| Mouse modes (1000/1002/1003/1006), focus (1004), bracketed paste (2004), synchronized output (2026) | ✅ | |
| Files dropped from Files and the clipboard's image paste their shell-quoted path (❖V; Ctrl+V in Claude Code) | ✅ | `apps/terminal/paths.js`; Claude Code attaches a pasted image path |
| Claude Code in the status bar: working or waiting for you, context used, the plan's 5-hour and weekly limits, model | ✅ | `apps/terminal/claude.js`, `ClaudeBar.jsx`, `lord-claude-status` (`server/src/term/claudebridge.c`); see "Claude Code in the status bar" |
| A color per tab (right-click a tab), over the profile's; the tab is tinted whole | ✅ | |
| Keys with modifiers: kitty keyboard protocol flag 1 (disambiguate; `CSI > u`, `< u`, `= u`, `? u`, a stack per screen) and xterm modifyOtherKeys 2 | ✅ | Shift+Enter is `CSI 13;2u` when a program asks (Claude Code: a new line); the other kitty flags (release events, all keys as escapes) aren't supported and the query says so |
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
| Save/restore window arrangement | ✅ | in the configuration file (`terminal.json`) |
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
| Ctrl/❖+click on paths and links | ✅ | paths under `$HOME` open Files; web addresses open in the default browser; anything else is copied |
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
| Triggers (notice, bell, attention, send text, capture, highlight, mark, title, stop) | ✅ | can be disabled one by one; edited in a sheet that tries them on a sample line (0.8.8.8) |
| Automatic profile switching by directory and by user@host | ✅ | user@host is reported by the shell integration (OSC 1337 RemoteHost) |
| Configurable keyboard shortcuts | ✅ | Preferences → Keys |
| Per-profile and global keys (send text, Esc+, hex, ignore, action) | ✅ | the profile's win over the global ones and the shortcuts |
| Send text on open, size of new windows in columns × rows | ✅ | several lines, each run with Enter, the last one too (0.8.8.8, `profilefields.js`) |
| Profile tags (menu groups) and tab names | ✅ | tags edited in General; a tab opened with a profile other than the default is named after it (0.8.8.8) |
| Import profiles from other terminal emulators | ✅ | see below |

### Not applicable or pending by design

| Feature of other terminal emulators | Status |
|---|---|
| Passwords when a program asks for one (ssh, sudo, git, gpg), and saving one typed by hand | ✅ since 0.8.8.6: a bar offers the Passwords logins (the host's first; it names the asking program, read from /proc); a typed password the program accepted is offered for saving. The value goes between lord-vault and the pty in C, never through JS (`src/term/termfill.c`, `tcapture.c`, docs/PASSWORDS.md) |
| Python API, AI assistant | — no scripting is exposed to the theme |
| Triggers that run scripts or coprocesses, custom per-profile commands | — LordOS only opens the user's shell; an imported "command" is typed into the shell on open |
| Background image, horizontal spacing | ⏳ |
| COLR/SVG emoji, ligatures with GPOS positioning (more precise marks over letters) | ⏳ |
| Automatic updates, Sparkle | — it's updated with LordOS |

## Claude Code in the status bar

While Claude Code runs in a pane, the status bar shows `✳ Claude Code · Opus 5.5 · ● working · ctx ▰▱▱▱▱ 24% · 5h 37% · week 61%`.
Colors: green up to 50 %, amber up to 80 %, red past it. A narrow window drops the model, then the week, then the 5 hours.
A click on a percentage shows the exact tokens, when each limit resets and the session's estimated cost.

- **Working / waiting for you:** `~/.claude/sessions/<pid>.json`, which Claude Code keeps for each running session. The pid
  is the pane's foreground process group (`term.info().fgPid`), and the file is used only when its `pid` matches.
- **Context and limits:** Claude Code's status line JSON (`context_window.used_percentage`, `rate_limits.five_hour`,
  `rate_limits.seven_day`; the limits exist only for Pro, Max and Ultra plans). It reaches the Terminal only after
  **Connect** (in the status bar, or a click on "✳ Claude Code"), which asks first and then puts
  `/usr/lib/lordos/bin/lord-claude-status` in front of the user's status line command in `~/.claude/settings.json`.
  A command with shell syntax goes whole through `sh -c`. Everything else in the file is kept, and the previous file is
  saved as `settings.json.lordos-backup`. An unreadable file is left alone. Disconnect gives the original command back.
- **lord-claude-status** (`server/src/term/claudebridge.c`, no libraries): Claude Code runs it, as the user, instead of
  the command it wraps. It keeps the JSON as `~/.cache/lordos/claude/<session_id>.json` (session ids are checked;
  0600 in a 0700 folder of the user's; no links followed; written whole and renamed; files older than a week removed),
  then runs the wrapped command with the same stdin, so the status line shows exactly what it did before. With no
  command to wrap (the user had no status line) it prints LordOS's own, the footer's numbers in one line:
  `Opus 5.5 | LordOS | main | ctx 11% | 5h 37% | sem 5%` (model, folder, git branch read from `.git/HEAD` without
  running git, context used, the plan's limits, the labels in the user's language; control characters and `|` from the
  input are left out). A subscriber's 5-hour window shows as `5h –` while Claude Code leaves it out (it drops a window
  once it resets), here and in the footer. lord-server runs nothing: the Terminal only reads those files.

- **Inside screen or tmux, or over ssh** (0.8.8.7): there the pane's foreground process is `screen`, `tmux`, `ssh` or
  `mosh-client`, not `claude`. In that case the Terminal reads the pane's screen (`term.text(sid, 'screen')`) every 2 s
  and looks for the last line in lord-claude-status's shape, `Opus 5.5 | LordOS | main | ctx 44% | 5h – | sem 26%`
  (`parseScreenStatus`). "esc to interrupt" on screen means it is working.
  - In a local screen or tmux, the session in `~/.claude/sessions/` whose snapshot has the same model, context and
    folder gives the exact state, tokens, resets and cost (`matchSession`). If none matches, or more than one could,
    the screen's numbers are shown as they are.
  - Over ssh, only the screen's numbers are shown. There is no Connect, because the remote's settings aren't ours. The
    remote's Claude Code must print that line, which is what lord-claude-status prints there. Installing it on a server
    from the Terminal comes with Files mode.
  - The footer shows `✳ Claude Code · screen` (or `tmux`, `ssh`).

Tests: `server/tests/terminal_claude_test.mjs` (the model, and that the wrapped command prints what the original did)
and `server/tests/claude_bridge_test.sh` (the bridge: what it keeps, bad session ids, a link in place of the folder,
the user's command and its exit status), both in `make test`.

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
changed (click on the shortcut and press the new combination). Changes are saved in the
configuration file (`shortcuts`, `keyMap`, and each profile's `keyMap`; see "Configuration file").

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

## Configuration file

(0.8.8.7) Profiles, color schemes, key mappings, shortcuts, the hotkey and the windows to restore live in their own
file, so they can be backed up, versioned or kept in a synced folder: `~/.config/lordos/terminal.json` by default,
`{ "schema": 1, "profiles": […], "defaultProfile", "hotkey", "schemes", "shortcuts", "keyMap", "live", "arrangement" }`.
Before 0.8.8.7 this was the `"terminal"` key of `settings.json`.

- **The move** (`apps/terminal/config.js`, at the Terminal's first use in a session): when `settings.json` still has
  `"terminal"` and `terminal.json` doesn't exist, `settings.json` is copied to
  `settings.json.pre-terminal-migration.bak`, the section is written to `terminal.json` and read back, and only then
  removed from `settings.json`. Every field is kept as it was (`itermGuid` too). If `terminal.json` is already there it
  wins and nothing is overwritten.
- **Another place**: Preferences → **Config File** → *Choose Location…* (a `.json` file in the home folder or on a
  mounted disk, for example `~/Backups/LordOS-Terminal/terminal.json`). If no file is there the configuration is moved
  to it; if one is, *Use the File That Is There* or *Replace It*. `settings.json` keeps only `"terminalConfigPath"`;
  every later change (profiles, colors, shortcuts, an import) is written there. *Back to the Default Location* moves
  it back. *Show in Files* opens its folder.
- **Writing** (`server/src/termconf.c`, `lord.termconf` in `ui/js_termconf.c`): a temporary file, `fsync`, `rename`,
  the folder `fsync`ed; always `0600` (a profile's initial text can name SSH keys). Before a write, when the newest
  backup is more than 10 minutes old, the backups move up (`terminal.json.1` … `.5`). A chosen file's folder is never
  created: a disk that isn't mounted must not get an empty configuration in its mount point. With a chosen file,
  `~/.config/lordos/terminal.json` is kept as a copy of the last good version (the cache).
- **When the chosen place isn't there** (a disk not mounted, a folder renamed, no permission, a file that isn't valid
  JSON or has a newer schema): the Terminal uses the cache read-only and says so in Preferences → Config File, with
  *Try Again* and *Choose Location…*; changes stay on screen and aren't written anywhere. Every 2 s it looks again and
  goes back to the file as soon as it is there.
- **Changed from outside** (a backup restored, a `git pull`): within 2 s the file is read again and the windows take
  it; the cache follows, and the chosen file itself is left exactly as it is. A deleted default file is written again
  from what is in memory.
- **Protected**: a profile can type a command when a session opens, so the active file, its backups and temporary
  files are refused to `lord.fs` (`fsops_protect_file`, wherever the file is), like `settings.json` and its migration
  backup. Only `lord.termconf` writes them; isolated apps never have it.
- **Export Profiles…** writes the whole configuration as JSON (`0600`, never over the active file); **Import
  Profiles…** takes that JSON (profiles replaced by id, schemes added). A profile that types into the shell by itself
  (an initial text, a `send` trigger, a key mapping that sends text) never replaces the default profile or becomes it:
  it comes in as a profile of its own, and the list says what it would type before importing. Global key mappings that
  send text are imported only when that option, off by default and listing the keys, is turned on or another terminal emulator's
  files (below), and writes the result to the configuration file.
- Tests: `build/termconf-test` (paths, missing and unavailable places, atomic `0600` writes, backups, the migration,
  protection) and `tests/terminal_config_test.mjs` (the move, read-only, choosing a place, refused files).

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
on a private unix socket (`$XDG_RUNTIME_DIR/lordos-term/`, `/run/user/<uid>` for a session of
lord-sessiond's, 0700). If lord-server ends because of a signal (restart, update) the shell keeps
running; on startup, the server connects to each holder (only from the same user, `SO_PEERCRED`)
and receives the PTY via `SCM_RIGHTS`. A holder gives its PTY to one lord-server at a time: while
the one that has it is alive (its control connection open), another lord-server of the same user
gets nothing (0.8.8.6; before, a test or headless lord-server took the user's shells too, read
part of their output and set their size to its own windows). A lord-server started with another
`XDG_RUNTIME_DIR` doesn't see the desktop's holders at all. Before
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

## Files mode, transfers and port forwards (0.8.8.7)

Status: Files mode is built (lord-sftp, term/sftpc.c, lord.sftp, apps/terminal/FilesMode.jsx and sftp.js; tested end to
end against OpenSSH by tests/sftp_test.c, tests/sftp_e2e.sh and on a real session). Port forwards are built (0.8.8.8,
apps/terminal/forwards.js and ForwardsPanel.jsx, checked on a real session against a private sshd), and so are uploads
by dropping on the terminal (dropupload.js, DropHud.jsx), the remote shell's directory and downloads from `ls`
output (lsnames.js, term.lineAt), and Open, Open With and Edit for remote files (remoteopen.js) (0.8.8.8).

Design approved by the maintainer on 2026-11-09 (mockups: the Terminal design canvas, boards 4 to 8). Everything uses
**the tab's own ssh session**: one login, no second connection, no password or code asked again.

### How the session is reused

- An `ssh` started by the Terminal (a profile whose command is SSH, or `ssh` typed in a shell with the integration
  loaded) gets `-o ControlMaster=auto -o ControlPath=$XDG_RUNTIME_DIR/lordos-term/ssh/%C-<shell pid> -o
  ControlPersist=no`. The folder is the user's and 0700. The integration adds these options to the ssh it runs, as a
  shell function (bash, zsh and fish; tested with bash 5.2, zsh 5.9 and fish 4.0.2: Files mode and a forward over each).
  The user's `~/.ssh/config` is never changed, an `ssh` that already sets `ControlPath`, `ControlMaster`, `-S`, `-O` or
  `-M` is left as it is, and `LORD_SSH_MUX=0` turns it off.
- `lord-server` knows which tab owns which control socket: the ssh is the tab's foreground process, and the master
  behind each socket in the folder says its pid (`MUX_C_ALIVE_CHECK`); the one whose pid is the foreground process is
  the tab's. Only names of that form (`<hex>-<pid>`) are asked. A socket that refuses the connect (its ssh was
  killed and left it) does not count against the 32 masters asked and is removed, by its name in the folder opened
  with no link followed, when it is still the user's socket; anything else in the folder is left as it is.
- To list, copy, download or forward, `lord-server` opens a **new channel** through the control socket with the
  OpenSSH multiplexing protocol (`MUX_C_NEW_SESSION` with the `sftp` subsystem and fd passing, `MUX_C_OPEN_FWD` for a
  forward). **It runs no program**: no `ssh`, `sftp` or `scp` is executed.
- The SFTP protocol is spoken by a new sandboxed helper, **`lord-sftp`**. It gets the channel's fds and, for a
  download or an upload, the local file's fd. It never gets a path, it is held by `sandbox_apply_helper()`, and it has
  no socket, connect, exec or clone. Replies from the server are untrusted data, so their parser has its own test and
  a fuzz case.
- **Remote commands** come only from a closed list, each with its arguments checked in C:
  - `file -b -- <path>` (Info)
  - `sha256sum -- <path>` (Info)
  - `sudo -n /usr/lib/openssh/sftp-server` or the `sftp-server` path the server reports ("As root")
  - `cd` is not one of them: "Open terminal here" types `cd -- '<path>'` into the tab, as the user could.
- **As root:** `sudo -n` first. If sudo needs a password, a sheet asks for it once per session; it is written to that
  channel's stdin and wiped, never kept and never sent to JS.
- Not reusable: an `ssh` opened before the update (its channel was not set up when it connected), and an ssh made from
  inside the server to another one (a hop). There the Files switch is grey and says why.

### Files mode

- **Turned on** from the footer's Files switch (right), Window → Server Files (SFTP) (❖⇧F) or the tab's right click.
  It changes the tab's view; the shell stays alive behind it.
- **Layout:** this computer on the left (the tab's local directory), the server on the right (the remote shell's
  directory, from OSC 7), and the transfers queue with a speed graph at the bottom. Uploads are orange and downloads
  blue (they also differ in lightness and arrow).
- **Toolbar:** New Folder, Refresh, Show Hidden, Filter, As Root.
- **Refreshing:** the local side refreshes at once (inotify). The remote side refreshes after each of our own
  operations and every 5 s while it is shown. Refresh is for changes made from elsewhere.
- **Right click on a remote item:**
  - Download, Open, Open With › (the app LordOS picks, then each Linux app that declares the type): the file comes down
    to a folder of its own under `~/.cache/lordos/terminal-remote` and opens there.
  - Edit: the file comes down the same way and opens in the Text Editor; each save (the copy's size or time, steady
    for one look, checked every 1.5 s) goes back up to the same path. It is the only upload that replaces a file
    without asking, since that file is the one the person chose to edit. It stops when Files mode closes or the
    connection ends.
  - Rename (F2), New Folder, Duplicate, Compress (.tar.gz)
  - Permissions (chmod, as boxes and octal), Change Owner (chown)
  - Info: `file`, size, owner, permissions, modified, and SHA-256 computed on the server
  - Copy Remote Path, Open Terminal Here, Delete…
- **A name that already exists:** Replace, Keep Both or Skip, with "apply to all".

### In terminal mode

- **Upload by dropping:** a file or folder dropped from Files onto a pane that runs ssh goes up to the remote shell's
  directory in the background (the remote home when the remote shell doesn't report one). A floating notice (it can
  be hidden) and the footer show the progress. Nothing is typed into the terminal and it is never blocked. A name
  already on the server is never replaced: the copy takes a free name (`notes (2).txt`) and a notice says so. Dropping
  with ⌥ pastes the path as text, as before, and so does a drop on a pane without ssh. Links are never followed (a link
  in a dropped folder could lead to `~/.ssh`): they are left out and counted in a notice. The notice shows the folder
  the files go to.
- **The remote shell's directory:** an OSC 7 that arrives while the pane runs ssh is the server's directory. It is
  kept apart from the local one (new tabs and the footer keep the local directory), and it is where drops go and
  where Files mode opens. It is forgotten when the ssh ends. Servers report it only when their shell does OSC 7. It is
  the server's output, so any program there can send one: it is kept only as an absolute path without control
  characters, and it is shown wherever it is used (the drop notice, Files mode's path bar).
- **Download from the output:** in a pane that runs ssh, ❖ + click on a file name printed by `ls` or `ls -l` downloads
  it to Downloads (a folder comes down whole, links are not followed, a name already there gets a free one), with the
  same notice in blue. A right click on a name offers Download and Copy Remote Path. The name is resolved against the
  remote directory from OSC 7, else the remote home. `term.lineAt(sid, x, y)` gives the line under the pointer split at
  its cell, and `lsnames.js` finds the name: `ls -l` lines by their columns (permissions to date, also long-iso and
  device numbers; "name -> target" for a link), otherwise the column under the pointer, with ls's quoting
  (`'notes (2).txt'`, `'it'\''s'`). Planned: underlining names under the pointer, and Download To…, Quick Look,
  Open With…, Edit, Info and Show in Files mode in the menu.

### Port forwards

- **In the profile, tab SSH:** a list of forwards, each "server host:port → 127.0.0.1:port". The server side can be
  `localhost:80`, or another machine the server reaches. The local port is **automatic** (LordOS picks a free one) or
  fixed. It listens on **127.0.0.1 only**.
- They open when an ssh started in a session of that profile connects (the pane's foreground process is `ssh`; a new
  ssh process starts the list again), through the same control socket (`MUX_C_OPEN_FWD`), and close with the
  connection. Until ssh has signed in there is no control socket yet: each forward waits for it (up to 3 minutes, for
  a password or a code), then opens. Profile → SSH edits the list: host (localhost by default), server port, local
  port (empty = automatic, else 1024–65535) and an on/off switch; the same checks as lord-server's.
- **On the tab:** a badge `⇄ 80→54321`. Clicking it lists each forward with its state and connections, with Open (in
  the browser, http/https), Copy and Close, and a field to add a forward for this session only.
- **When connected:** a notice with the address and Open in Browser / Copy. The footer shows how many forwards are open.

## Security

- (0.8.8.7, in development) Files mode, transfers and forwards start **no** process: they open channels on the tab's
  own ssh control socket (OpenSSH multiplexing protocol), and the SFTP replies are parsed by the sandboxed `lord-sftp`
  helper. Remote commands come only from a closed list (`file`, `sha256sum`, `sudo -n sftp-server`). Forwards listen on
  127.0.0.1 only.
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
