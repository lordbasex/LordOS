# Hands: an AI agent uses the LordOS desktop (in development)

**Hands** (Manos) lets an AI agent use the LordOS desktop the way **Claude in Chrome** lets Claude use a browser: it
reads the windows, clicks, types, scrolls and takes screenshots. It is for **Claude Code** today and for **Lumo**'s own
agent later. The maintainer approved the design on 2026-11-26 (AGENTS.md).

LordOS's desktop is drawn by lord-server from React. Every button, field, menu and list row is a node in a tree that
lord-server and the shell both know, with its text and its box. An agent can read that tree as Claude in Chrome reads a
page's accessibility tree, instead of guessing from pixels.

## Add it to Claude Code

```sh
claude mcp add -s user lordos-mcp -- lordos-mcp
```

`-s user` adds it for every project (without it, Claude Code keeps it only for the folder the command ran in). Settings →
AI → Hands shows this command with a Copy button (`<CommandBlock>`, `sdk/codeblock.jsx`); LordOS only copies it, it
never types or runs it.

Or in a project's `.mcp.json`:

```json
{
  "mcpServers": {
    "lordos": { "command": "lordos-mcp", "args": [] },
    "lordos-qa": { "command": "lordos-mcp", "args": ["--headless", "notes"] }
  }
}
```

Then turn on **Settings → AI → Hands** (for this session). The first time Claude Code acts, LordOS asks you in a system
prompt: **This session** or **Deny**.

## The tools

They mirror Claude in Chrome's. `lordos-mcp` (`server/src/cli/lordosmcp.c`, schemas in `server/src/hands/mcp.c`) is an
MCP server over stdio; each tool is one or two calls to lord-server's `org.lordos.Hands1`.

| Claude in Chrome | lordos-mcp | What it does |
|---|---|---|
| `tabs_context` | `windows_context` | the windows: id, app, title, display, focused, minimized, kind (native, linux, package), frame |
| `tabs_create`, `navigate` | `open_app(app)` | opens an app by id or shown name; answers its window id |
| `navigate` | `open_path(path)` | a file or folder of the home folder or a disk in its default app, through `openFileWithApps` with `untrusted` (a file that would run code is only shown in Files) |
| `tabs_close` | `close_window(window_id)` | closes a window (the app may ask about unsaved changes) |
| `read_page` | `read_window(window_id, filter, depth)` | the UI tree as text (below) |
| `find` | `find(window_id, query)` | words matched against roles, names and values; the best lines with their refs |
| `computer` | `computer(action, …, mode)` | `screenshot` (`display`, `display:<id>`, `window:<id>`), `left_click`, `right_click`, `middle_click`, `double_click`, `triple_click`, `hover` (at a `ref` or a `coordinate`), `left_click_drag` (`start_coordinate` → `coordinate`), `type` (`text`), `key` (`text`: `ctrl+s`, `Return`, `ctrl+a Delete`; `repeat`), `scroll` (`scroll_direction`, `scroll_amount`), `zoom` (`region` [x0, y0, x1, y1]), `wait` (`duration`, at most 10 s); `mode` `human` or `machine` for this call (below) |
| `form_input` | `set_value(ref, value, mode)` | a text field or area, a checkbox or switch, a pop-up or option list (by value or label), a slider; never a password field |
| — | `hands_mode(mode)` | `human`, `machine` or `default` for the rest of the session (also `lordos-mcp --mode human\|machine`) |
| `read_console_messages` | `read_app_log(app, pattern, max_lines)` | an app's own log: its file in the session's log folder (`apps/<app>.log`, a web app's `web/<app>-*.log`), else its lines of `shell.log`, through `lord.logs` (the Logs app's own access rules) |
| `gif_creator` | `record(start \| stop \| export, name)` | every action adds a frame; `export` returns the animated GIF, or with `name` (a bare file name ending in `.gif`, no folders) saves it as a new file in `~/Pictures/LordOS QA`, the only place lordos-mcp ever writes (each folder opened `O_DIRECTORY \| O_NOFOLLOW`, the file `O_CREAT \| O_EXCL \| O_NOFOLLOW`, 0600) |

There is **no `javascript_tool`**: nothing runs code. lord-server never runs a command for an agent.

Coordinates are pixels of the last screenshot (desktop points before the first one); `lordos-mcp` maps them back with the
screenshot's area (`{x, y, w, h, width, height}`: the part of the desktop it really shows, so a window that runs off the
screen maps right). Screenshots are at most 1,568 px on their long side by default; a zoom may grow up to 4 times. A
window's screenshot includes its sheets (Settings → Wallpaper's sheet, a file dialog: `wm.addSheet`), as Screenshots'
window capture does. A zoom is clipped to the desktop; only a region wholly off it is refused (`outside`).

## The agent's cursor

The person sees the agent work, as Claude in Chrome shows its own cursor moving to each target.

- **Its look**: a violet arrow (a rounded tip at the point) with a small label, the name the client gave followed by
  "· reported" (`hands.cursor.label`): what the program calls itself, not verified, as the prompt says. The name was
  cleaned in C when the client said Hello (printable UTF-8, no control, bidi or format characters, 40 bytes at most) and
  is cut again in the shell; it is plain text. A ripple when it clicks, a short pulse at the field while it types or
  presses keys, a quick ring where a machine-mode action lands. It fades out 4 s after the last action and goes at once
  with Stop, the panic key or the last agent leaving.
- **Where it is drawn**: lord-server draws it from C (`server/src/hands/handscur.c`) in a layer of its own at zIndex
  99.5, built once by the shell out of React's sight (`sdk/hands.jsx` `HandsCursor`, `runtime` `overlayTree`): above
  every layer a theme draws in (notification, 95) and below the system band (100), so a permission or password prompt
  and Hands' banner are always above it. It takes no clicks. Each frame of a glide changes only its position: the damage
  is its own box, with no JavaScript per frame. It is on whatever display its point is.
- **Separate from the person's pointer**, which never moves for the agent: the agent borrows the pointer's position for
  its input, the person's pointer image stays where they left it, and their next pointer input of their own (a move, a
  button, a wheel, a remote viewer's) takes it back from there before it is applied (`main.c` `hands_reclaim`).
- **Not in the agent's screenshots**: the agent sees the screen as it is, with nothing of its own over the target
  (`paint_hands_cursor`), and it is not in `read_window`'s tree.

**Human mode** (the default on the live desktop): before a click, a hover, a drag, a scroll or `set_value`, the cursor
glides from where it is (the first time, from the person's pointer) to the target along a slightly curved path, eased in
and out, in 250 to 900 ms by Fitts's law (longer for a far or small target, `server/src/hands/handsmotion.c`); it hovers
90 to 170 ms (the pointer is over the target, so its highlight shows), then the action happens. A drag carries the
cursor from start to end with the button held; a scroll comes in up to 4 steps; text is typed one character at a time,
25 to 60 ms each with a small jitter (a long text faster: at most 10 s a call), the cursor resting at the field's end.
With Settings → Accessibility → Reduce motion the glides are short (at most 120 ms) and straight. **Machine mode**
(the default of the `--headless` QA desktop): every action at once, as before; the cursor only jumps and flashes where
it lands.

**Choosing it**: Settings → AI → Hands → *Agent motion*: *Human* / *Machine* (`settings.hands.motion`, on the
`handsDeny` page, so only the person changes it; `lord.hands.motion`); a client's own choice wins for that connection:
`hands_mode` (`SetMode` on the bus) or `lordos-mcp --mode`, and `mode` on one `computer` or `set_value` call.

**What it never weakens**: the glide itself injects nothing; the hover move and the action pass the whole gate at the
moment they happen (a target that became protected, covered or refused meanwhile is refused then); each scroll step and
each typed character passes the gate again (the full key gate before every character); a drag's drop is gated on
arrival, and if refused the button is let go where the drag began. The person's input stops an action in progress at
once ("user is active"; a held button released) and still pauses the agent 3 s; Stop, the panic key, the lock and the
switch end it. One action runs at a time (another client's input gets Busy).

### read_window

```
window "Notes" id=notes [ref_1] @185,122 860x560
  clickable "File" [ref_7] @194,161 36x22
  textfield "Search notes" [ref_13] value="" @196,197 186x28
  button "New Note" [ref_14] @388,197 28x28
  row "Shopping list" [ref_40] {selected} @196,240 210x28
  protected "Password"
```

One line per thing a person sees or uses: a **role** (window, button, textfield, textarea, checkbox, switch, radio, tab,
menuitem, row, option, combobox, slider, link, text, image, app, terminal, clickable…), a **name** (its label, its text,
its placeholder or tooltip), a stable **`ref_N`** (the same for a node while it lives), **state** (disabled, checked,
selected, expanded, busy, focused), a **value** for fields that aren't secret, and its **box** in desktop points.
`window_id` `""` is the window in front with the menu bar, menus and sheets; `"desktop"` is every window and shell surface.
`filter: "interactive"` keeps only what can be used.

Roles come from the SDK's controls (`accessibilityRole`, `accessibilityState`, `accessibilityValue`, set on Button,
Toggle, Checkbox, RadioGroup, Tabs, Segmented, Dropdown, TextInput, TextArea, Slider, SidebarItem, the menus), else from
what a node does (a click handler or `focusable`: `clickable`). A Linux app's window is one `app` line: it has no tree in
LordOS; take a screenshot of it. An isolated `.lpk` app runs in its own process: the same.

## Security model

### The connection

- `lordos-mcp` (`/usr/bin/lordos-mcp` → `/usr/lib/lordos/bin/lordos-mcp`) runs as the person, links libc only (and
  vendored stb_image to decode lord-server's own PNGs for a recording), and opens nothing of the desktop itself.
- It calls **`org.lordos.Hands1`** on the session bus (`/org/lordos/Hands1`), served by lord-server on the portal's
  connection (`server/src/ui/handsbus.c`). The bus says each caller's uid (`GetConnectionCredentials`) before its first
  call is served; another uid gets AccessDenied. Methods are closed and typed: `Hello(s)`, `Status()`, `Windows()`,
  `ReadWindow(ssu)`, `Find(ss)`, `Pointer(ssiiii)`, `Scroll(siisu)`, `Key(su)`, `Type(s)`, `SetValue(ss)`,
  `Screenshot(siiiiu) → (ay s)`, `OpenApp(s)`, `OpenPath(s)`, `CloseWindow(s)`, `AppLog(ssu)`, `SetMode(s)`. In human mode
  `Pointer`, `Scroll`, `Type` and `SetValue` answer when the action is done (the cursor arrived), not at once. Every argument is checked
  in C (`handsbus_parse`, fuzzed by `build/hands-test`) before the policy sees it.

### Consent and control

- **Off** until the person turns it on for a session: Settings → AI → Hands (`lord.hands.serve`, held in lord-server's
  memory, never on disk; a file an agent could edit can't turn it on).
- **One prompt per connection**, drawn by the system layer when a client first says Hello: "A program running on your
  account asked to control your computer", the name it gives and its program path, both marked as reported, not
  verified, and **This session** / **Deny**. Only the person's real input answers it (the agent can't reach the system
  layer). The grant is that connection's: it ends when the connection closes or its process exits (lord-server keeps
  the bus's `ProcessFD` pidfd only for that), and a new connection asks again.
- **The banner**: while a granted agent is connected, every display shows "<client> is using your computer" with
  **Stop**, in the `agent` layer (105, part of the system band, above every layer a theme draws in; it doesn't take the
  keyboard).
- **The panic key**: **Ctrl+Alt+Esc** on a real keyboard (`on_input`, before any JavaScript, the keyboard asked whether
  Ctrl and Alt are really held) stops every agent and turns Hands off for the session. Locking the screen, switching
  user and turning the switch off stop every agent too.
- **The person's input wins**: for 3 s after a real key, click or move (or a remote desktop viewer's), the agent's input
  is refused with "user is active".
- **An allowlist** (Settings → AI → Hands, "Only these apps"): the agent reads, clicks and types only in those apps'
  windows (and the menu bar, menus and sheets while one of them is in front), opens only those apps, and its
  screenshots are a window or a region entirely inside one allowed window, with every other surface black (fail closed:
  everything but the allowed windows and the menus, popovers and sheets tied to them by `handsWin`).
- **Rate limits**: 8 input actions a second (bursts of 16), 10 reads, 2 screenshots (bursts of 4).
- **Logged**: each call is a line of closed words in `shell.log` (`hands: claude pointer left_click refused password`),
  never text, names or values.

### Never

- the **system layer** and everything above it (`sdk/layers.js` 100 and up: permission, keyring, password, polkit and
  Bluetooth prompts, the portal's file chooser for Linux apps, Screenshots' selection, the on-screen keyboard, the banner
  itself): no click, key, read or pixel. While a system prompt shows, no key at all.
- **password fields**: never typed into, never read (no value in the tree, `set_value` refused in C and in the shell, a
  Linux app's field that says it is a password or PIN through text-input-v3 refused too), and **protected** windows and
  fields (`protected`: the vault, password fields): no click, key or read.
- nodes marked **`handsDeny`**: Settings → AI → Hands (so an agent can't turn itself on, widen its allowlist or answer
  for the person) and the theme-trust sheet.
- **the person's own answers**: every destructive confirmation (`ConfirmDialog` is `handsDeny` when `destructive`:
  deleting files for good, emptying the Trash, removing machines, packages, web apps and themes…), the theme-trust
  dialog, Machines' automation permission, encryption choice and recovery key, signing out (`logOut` refuses an agent's
  click or key) and the power dialog (an agent's input never confirms it; its countdown cancels instead), and, listed in
  one place (`sdk/handsdeny.js`, checked by `tests/hands_deny_test.mjs` so a dead key fails), whole Settings pages
  (Network with its Wi-Fi and VPN credentials, Sharing, Cloud, Privacy with the firewall and its rules, Users, Web apps,
  Flatpak, Updates, Session) and whole apps (Store, Passwords, Backups, Disks, Imager, the installer, Remote Desktop). While a dialog that owns the
  keyboard (`dialogStack`'s top, whose root `ConfirmDialog` and the power dialog record) is `handsDeny`, protected or
  in the system band, or its root isn't known, **every** agent key is refused: "a protected dialog is open"
  (`protected-dialog`), checked in the shell and again in C on the dialog's root.
- **security-state natives refuse an agent's input natively** (`hands_native_guard`, applied to every `lord.<ns>.<fn>` in
  `js.c` `prof_call`, and again where each action runs: `term.fill`, the firewall's setters, `perms.set`,
  `vault.request`, and the broker's keyring, FIDO key and secret-to-clipboard requests of isolated apps). Fail closed:
  while a Hands client is active and the agent's input came after the person's last real click or key
  (`hands_agent_owns_input`; motion never counts, time never clears it), the vault and keyring, `term.fill` and saving a
  terminal secret, the firewall's changes, permissions, administrator actions, Sentinela's blocklist, theme trust and
  removal, signing keys, screen-sharing answers, Hands' own switch and power refuse (`hands.refused`), so a surface
  missing its `handsDeny` can't reopen them. **One exception, catalog wallpapers** (maintainer-approved 2026-11-26,
  AGENTS.md): `lord.admin.request` is decided in C (`hands_admin_allowed`, `js.c` `prof_call`) and an agent's input may
  set off only `wallpaper-get "<id> <variant>"` (Settings → Wallpaper's "Download and use" and "Download all"),
  `wallpaper-remove "<id> <variant>"` ("Free up space"), `wallpaper-login` with `default`, `still <id>`,
  `still cat:<id>` or `<id> <variant>`, and Studio's twin `studio-bg-get "<id> <variant>"`: ids `[a-z0-9-]`, variants
  `[a-z0-9.]`, one space, nothing else, both values already JS strings (never an object whose `toString` could change
  between the check and the call). Why: each names an entry of a closed catalog in the package that root reads
  (`server/src/hw/wallpaper_admin.c`, `studio_bg_admin.c`), never a URL or a path, and root keeps a file only at the
  catalog's size and SHA-256; at worst an agent downloads, uses or deletes a stock wallpaper. A person's own picture
  (`wallpaper-image`, and `wallpaper-login-image`, which shows it on the login screen to every account), Studio's
  removal and every other action stay refused, and so does anything that doesn't parse. Each such request is logged as
  closed words: `hands: claude admin wallpaper-get ok`, `hands: claude admin other refused`. While any Hands client is active, a secret on the clipboard is nobody's:
  `lord.clipboard.get` answers "", the broker gives an app nothing and the Wayland clipboard is not offered; an agent's
  paste (by the key it resolves to, `KEY_V` with Ctrl or Super, Shift+Insert, `KEY_PASTE`) and middle click are refused,
  and an agent's input is never an isolated app's paste gesture. The Terminal's Passwords and save bars, Monitor's firewall switch, rule sheet and
  blocklists and Sentinela's blocklist Remove are `handsDeny` too; a program asking for a password in the Terminal gets
  no agent key.
- **keys**: the whole key gate runs before every chord and repeat of a Key call and before each Tab or Enter of a Type
  call; a person-only app in front takes no agent key; paste chords and a middle click are refused while the clipboard
  holds a secret (`sensitive-clipboard`); an agent's Tab skips person-only controls and their keys are swallowed.
- **sandboxed callers**: Hands1 answers only calls addressed to `org.lordos.Hands1` (never through the portal's names,
  which a Flatpak's D-Bus proxy lets through; Writing1 and WebApps1 likewise), and refuses a Flatpak app or its proxy
  (`/.flatpak-info`, an `app-flatpak-*` cgroup, `xdg-dbus-proxy`).
- the **lock screen** and the **greeter**: other processes; Hands1 is never served there.
- an **authentication prompt** (polkit, `pkagent`) takes only physical input: no agent click or key while one is up.

Screenshots of the system band and `handsDeny` nodes are painted solid black over their whole box, in every capture path
(a display, a window, a region), and so are a marked window's Linux menus (`server/src/hands/handsmark.c` marks them
`hands_black`, not `protect`, for the length of the capture); when not everything can be marked, there is no screenshot.
A window's screenshot is refused only when the window itself is protected: a protected node inside it (a password field)
is painted as the protected fill and the rest is shown.

### Threat model

Programs of the same user are one trust boundary, as for any desktop automation: one can claim any name, run inside any
program (fork and exec, `LD_PRELOAD`, ptrace), or edit the user's files. LordOS doesn't pretend to tell them apart: the
prompt shows the client's name and program path as **reported, not verified**. The controls are the person's own
per-session consent on a prompt only real input answers, the visible banner with Stop, the panic key, pausing on real
input, the closed list of what an agent can do, the system layer and password exclusions, and the optional allowlist.

## The input path

The agent's pointer, wheel and keys go through lord-server's own input path, the remote desktop's (`main.c`
`hands_move`, `hands_button`, `hands_wheel`, `hands_keyev`: `remote_settle` → `dispatch_pointer` → hit test, `js_key`),
so focus, menus and Linux apps (Wayland and X11 through Xwayland) get it like real input. Never `/dev/uinput`, ydotool or
evdev. Text is typed through the active layout as the screen-sharing API types (a key per character, dead keys, an
input method's commit for what no key types; a Linux app's text-input field gets one commit); a line break and a tab are
keys of their own, and the field is checked again after each. lord-server tags it as the agent's: `lord.hands.injecting()`
and `status().events` count it. Every action passes **one gate** (`hands.c` `gate`): the ref's own node and the node under
the point (system band, protected, `handsDeny`, an authentication prompt), the ref really under the point (not covered),
never a secret field, the allowlist; for keys, the field and the node with the keyboard focus and every node they are in.

## QA mode: `lordos-mcp --headless APP`

The safest way to test an app with an agent: `lordos-mcp --headless notes` starts a **private desktop** nobody sees: a
private `dbus-daemon`, `lord-server --headless 1280x800 --hands-qa` and a demo home with a few sample files
(`Documents/Shopping list.md`, `Meeting notes.txt`, `Ideas.md`), then opens the app. The person keeps working on their own
desktop; nothing of theirs is read or touched. It has a demo identity everywhere the account could show: "Alex Demo",
login "demo", host "demo-pc" (`lord.system.info`, `lord.users`, the environment, the terminal's prompt); `tests/hands_e2e.py`
fails if the real account or host name appears in its trees. `--hands-qa` is accepted only with `--headless` (never with a monitor or
on the login screen): Hands is on and every client granted with no prompt, and there is no person to yield to. It all
ends, and the folder is deleted, when `lordos-mcp` exits. `LORDOS_MCP_SIZE` (e.g. `1920x1080`) and `LORDOS_MCP_LANG`
change the desktop. It needs `dbus-daemon` (Debian's `dbus-daemon` package).

## Code

| Where | What |
|---|---|
| `server/src/hands/hands.c` | state and policy: clients, the gate, rate limits, the calls, the log's words, human mode's actions over frames |
| `server/src/hands/handsmotion.c`, `handscur.c` | human mode's motion (Fitts's law, the curve, typing's pace); the agent's cursor on the screen |
| `server/src/ui/handsbus.c` | `org.lordos.Hands1`: the uid, the pidfd, the arguments |
| `server/src/main.c` | the host: where a point or a key lands, the injection, screenshots, the panic key, `--hands-qa` |
| `server/src/hands/handsmark.c`, `handsimg.c` | what a screenshot hides (fail closed); fitting and PNG |
| `server/src/ui/js_hands.c` | `lord.hands` (desktop context only: never a community theme's or an isolated app's) |
| `sdk/handsmodel.js`, `sdk/hands.jsx` | the reader and refs; the shell's answers, the prompt, the banner, the agent's cursor |
| `apps/settings/AI.jsx` | Settings → AI → Hands |
| `server/src/cli/lordosmcp.c`, `server/src/hands/mcp.c` | lordos-mcp |

## Tests

- `build/hands-test` (`make test`, ASan and UBSan): the pure checks, the policy against a fake host (the prompt per
  connection, Stop, the panic key, the person's input, the system layer, a covered ref, set-value's gate, keys into a
  password field or a denied focused node, allowlisted screenshots, a window's sheets, a region clipped to the desktop),
  human mode (Fitts's law, the curve, typing's pace and budget, a scroll's steps; a click pending until the cursor
  arrives with nothing injected during the glide, the person's input stopping it, a target protected meanwhile refused on
  arrival, a drag's button released when interrupted, typing stopped when the focus moves to a password field, one
  action at a time, SetMode, the QA default, Stop hiding the cursor), a fuzz of every method's arguments, the bus's uid
  check on a private `dbus-daemon`, the pidfd, the screenshot's marks (black, not `protect`; forty denied rows; failing
  closed), lordos-mcp's pure parts, the catalog-wallpaper admin allowlist (the allowed actions with valid arguments;
  a person's own picture, other actions, a name with a prefix or a suffix, extra spaces, paths, URLs, a NUL refused) and
  its log line.
- `tests/hands_model_test.mjs`: refs stay stable, password values and the system band never appear, `handsDeny` nodes
  are left out, the allowlisted blackout per layer, the prompt claims nothing is verified.
- `tests/hands_e2e.py` (run by `make test` when the themes are built): `lordos-mcp --headless` with the test hooks:
  Notes' File → Import… of a fixture file (the note exists; a GIF recorded, returned and saved only under a bare name in
  `~/Pictures/LordOS QA` of a temporary HOME); a click and a key aimed at a permission
  prompt refused and the prompt black in screenshots; the banner on the display and the person's Stop ending the agent;
  Enter and a click on a community theme's trust dialog refused, the theme left untrusted; Monitor's Network tab
  captured as a window (it used to fail) and as the display with its firewall switches black in both; a window dragged
  off the screen's edge, its screenshot and a zoom at the edge; a wallpaper's sheet in Settings' window screenshot and a
  zoom over it; the agent's cursor on the display while it glides and where it clicks, never in the agent's screenshot,
  the click landing after the glide, the person's pointer not moving (`where`, a test hook).
