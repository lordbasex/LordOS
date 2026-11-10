# Design guide for LordOS apps

LordOS apps follow our design guide, modeled on widely used desktop human-interface conventions: generous whitespace, a small type scale,
consistent control heights and one accent-colored action per view. This guide is the reference
for every polish pass, so all apps share the same spacing, type and controls. The shared building
blocks are in `sdk/controls.jsx` and `sdk/components.jsx`, and the tokens below are exported from
the SDK as `spacing`, `typeScale` and `controlSize`.

The first pass (Calculator, Text Editor, Notes, Photos, PDF Viewer and Fonts) and the second one
(Terminal, Store, Monitor, Player, Screenshots, Camera, Voice Recorder, Scanner, Emoji & Symbols
with the system emoji picker, About and the Installer) are documented with before/after
screenshots in [`docs/polish/`](polish/), as is the third (Settings page by page, Files, Terminal
Preferences and the Store's remaining pages), and the fourth (the system apps: Disks, Backups, Logs,
Clock and Help), as is the fifth (Mail, Calendar, Contacts and Reminders) and the last one (Passwords).

## Spacing: an 8 px grid

| Token | px | Use |
|---|---|---|
| `xs` | 4 | tight spots: between a glyph and its label, between grouped toolbar buttons |
| `s` | 8 | between controls in a row, between rows of a list, toolbar gap |
| `m` | 12 | toolbar side insets, inside a grouped box, between a label and its control |
| `l` | 16 | padding of panels and dialogs that sit inside a window |
| `xl` | 20 | **window content inset** (nothing touches the window edge), dialog padding |
| `xxl` | 24 | between groups and sections |

Rules:

- The content of a window starts 20 px from its edges. Lists in a sidebar start 10 px in, with
  8–10 px of padding inside each row.
- Separate groups by 20–24 px and controls by 8–12 px. Never place two controls touching.
- Use hairlines (1 px, `separator`) between a sidebar and its content, under a toolbar and above a
  status bar. Don't use thick borders.

## Type scale

Inter, weights 400 / 500 / 600 only (700 only for very short emphasis).

| Token | px | Weight | Use |
|---|---|---|---|
| `caption` | 11 | 400–600 | secondary labels, section captions in sidebars, status bars, counts |
| `callout` | 12 | 400 | hints, secondary text in rows |
| `body` | 13 | 400–500 | default text, list rows, buttons, menus |
| `headline` | 15 | 600 | dialog titles, month headers, empty-state titles |
| `title` | 17 | 600 | the title of a pane when space is short |
| `largeTitle` | 22 | 600 | the title of a library or detail view (Photos, Fonts) |

Secondary text uses `textDim`. Avoid uppercase captions: use sentence case ("Contents",
not "CONTENTS").

## Controls

| Control | Height | Radius | Notes |
|---|---|---|---|
| `Button` | 28 (regular), 22 (`small`) | 7 / 6 | `primary` = the one accent action of the view; `plain` = borderless; `destructive` |
| `TextInput` | 28 (13 px text) | 7 | grows with `size`; `secure` = a password field: an eye at the trailing edge shows the text until the field loses focus (`eye={false}` when the view has its own Show), and ⇪ "Caps Lock is on" before it while it has focus with Caps Lock on |
| `SearchField` | 28 | 7 | magnifier at the leading edge, clear button at the trailing edge |
| `Segmented` | 28 (22 `small`) | 8 | `appearance="neutral"` for view switchers (Read/Edit, Sample/Characters/Info); `fill` to share the width |
| `ToolbarButton` | 28 (22 `small`) | 6 | icon-only items **must** have a `title` (tooltip); `active` = toggled on |
| `ToolbarGroup` | 32 | 8 | buttons that belong together: zoom −/value/+, markup tools |
| `Toolbar` | 44 | — | 12 px side insets, 8 px gap, hairline at the bottom |
| `SidebarItem` | 28 | 6 | source list row; string `icon` is drawn in the accent color; optional `detail` |
| `MenuButton` | 28 | 8 | "⋯" or "⊕" button that opens a small menu (checked, disabled and destructive items) |
| `Toggle` | 24 (20 `small`) | — | `small` = the 36 × 20 switch of grouped forms (Settings rows) |
| `Hairline` | 1 | — | `edge="top" \| "bottom" \| "start" \| "end"`: a 1 px separator on that edge of its parent; start/end follow RTL |
| `Dropdown` | 26 (22 `small`) | 7 | a pop-up button over `options = [[value, label]]`; a list of 24 px rows that scrolls after 12 |
| `Checkbox` | 14 box | 4 | `checked`, `mixed` (a dash), with its label |
| `RadioGroup` | 14 dot | 7 | `options = [[value, label, { disabled }?]]`, one under the other or in a `row` |
| `Tabs` | 30 | — | a row of tabs, the chosen one underlined in the accent, a hairline under the row |
| `Popover` | — | `menuRadius + 2` | a box below (or above) a control, kept inside the window; a focus scope |
| `Tooltip` | 22 | `menuRadius − 2` | wraps any control: hover or keyboard focus, after 550 ms |
| `ProgressBar` | 6 | 3 | `value` 0–1, or none for the indeterminate band |
| `List`, `Table` | 26 rows | 6 / 10 | selection by the pointer and the keys (`multiple`: Shift ranges, Ctrl toggles), type-to-find; a `Table` has a header that sorts (`sort`, `onSort`) |

Icons are monochrome Unicode glyphs from the fallback fonts (Inter, DejaVu Sans, Noto Sans
Symbols 1/2), not color emoji. Verified on the VM:
`✎ ⧉ ⊕ ⋯ ◫ ⛶ ⤢ ⟲ ⟳ ↻ ⎙ ⌕ ✕ ◐ ▬ ▤ ▦ ▶ ⓘ ☺ ‹ ›`. Color emoji (🗑 📎 🖼 🗨) and some
symbols (🗀) don't render as icons: draw a small shape instead (see `FolderGlyph` in Photos).
The media symbols (⏮ ⏸ ⏭ 🔁 🔊 💬 🎙 📷) are color emoji too: the Player draws its transport
glyphs (`PlayGlyph`, `PauseGlyph`, `SkipGlyph`, `SpeakerGlyph`, `CaptionsGlyph`), the Camera and
the Recorder draw their empty-state icons. `↻ ⤮ ♪ ◫ ▣ ☺ ≡` render monochrome.

## Layout patterns

- **Sidebar (source list):** 200–240 px, `sidebarBg`, a hairline on its trailing edge,
  `SectionTitle` captions (11 px, 600, `textDim`) and `SidebarItem` rows. A search field goes at
  the top of the sidebar when the list is what you search.
- **Toolbar:** icon-only buttons with tooltips where that is the convention. Show a text label only for
  the main mode switch or the back button. Push trailing items to the end with `ToolbarSpacer`. In
  narrow windows, hide items that are also in the app menus (print, invert, rotate), and move
  secondary actions into a `MenuButton` ("⋯"), so the toolbar never wraps to a second row.
- **Grouped form ("inset"):** `GroupBox` with `FormRow` children: a rounded box (radius 10) with
  hairlines between rows. Rows are at least 32–36 px. In a key/value list, put the labels in a
  right-aligned column (`labelWidth`) and the values leading-aligned.
- **Empty state:** `EmptyState` with an icon, a short title (15/600) and one hint line (12,
  `textDim`), centered. At most one primary button.
  For an outcome worth a little warmth (an error or an unavailable service, "everything is up to
  date") pass `mascot` instead of the icon: `mascot="confused"`, `"celebrate"`, `"shield"` or
  `"wave"` draws Lumo at 112 px (`sdk/mascot.js`, docs/LORDOS-BRAND.md). Not in "nothing selected"
  panes, and never more than one Lumo per view.
- **Dialogs and sheets:** 20 px padding, title 15/600, body 13 with `lineHeight: 1.4`, buttons at
  the end of the last row with 12 px above them. The default action is `primary`.
- **Hierarchy:** one primary (accent) action per view. Everything else is plain or neutral.
- **Inspector / detail pane:** a fixed-width pane (200–280 px, `sidebarBg`, `Hairline edge="start"`)
  shows an `EmptyState` until something is selected, never a blank column (Emoji & Symbols).
- **Media bar:** the Player's now-playing bar spans the whole window: cover and title at the
  start, the transport (buttons above a progress row) in a 380 px center column, the volume at
  the end. Under 720 px it keeps only the cover and the transport. Measure the box after layout
  (`lord.getLayout(ref.current.id)[2]`) to adapt columns and widths (Player, Recorder waveform).
- **Viewfinder apps (Camera):** always dark, whatever the theme; mode switch as two words with
  the current one in yellow, a 60 px shutter centered between two `flex: 1` columns.
- **Wizards (Installer):** step list in the sidebar (numbered circles, the current row
  highlighted), `largeTitle` step title, fields with 16 px between them, a footer with a hairline
  and Back / Next (`disabled` instead of a faded look).
- **Utility (Disks):** the actions for the selection are labelled `ToolbarButton`s over the detail
  pane (the ones that don't fit go into "⋯"), the details are a `GroupBox` of `FormRow`s with a
  right-aligned label column, and the selection title is `largeTitle`.
- **Console (Logs):** one 44 px toolbar (live, export, process field, `SearchField`), a 40 px scope bar
  with small neutral `Segmented` filters, column captions over the list, a detail pane and a status
  bar, each separated by a `Hairline`.
- **Settings plus browser (Backups):** a header with the app icon, status and the one primary action;
  a neutral `Segmented` centered in a 44 px bar; settings as `GroupBox`es in a 640 px centered
  column; the browser as a dated source list plus a file list with column captions.
- **Tabbed utility (Clock):** the tabs in the toolbar, centered, and the "add" action as a "+"
  toolbar button at the trailing edge, never a button floating at the bottom.
- **Reader (Help):** a centered reading column up to 680 px; the article keeps its own direction
  (`textDirection(body)`), so an English article in an Arabic interface still reads left to right.
- **Mail / Calendar / Contacts / Reminders:** source lists use monochrome glyphs in the accent
  color; where the glyph would be a color emoji (tray, archive, trash, folder, person) it is drawn
  with views (`Glyph` in `apps/mail`, `PersonGlyph` in `apps/contacts`). Mail keeps three panes with
  an icon-only toolbar over the list and the message; list rows are 86 px (sender, subject, two lines
  of preview) with a rounded selection, and under 1000 px the panes shrink to 200 / 290 px.
  Calendar centers the Day/Week/Month switch, puts the period title (22/700) over the grid with
  ‹ Today › at its trailing edge, and hides the sidebar below 760 px unless asked for. Bottom bars
  of adjacent columns share one height (40 px) so their hairlines line up. Text that belongs to the
  user (event titles, previews) keeps its own direction (`textDirection`) in an RTL UI.
  `ToolbarButton`'s `icon` may also be an element (a drawn glyph).
- **Settings (System Settings):** a 220 px sidebar (search field, the user card, groups of
  `SidebarItem` rows 12 px apart with 20 px colored badges: a soft vertical gradient and a white
  glyph, or a drawn shape where the glyph is a color emoji), a 48 px title bar over the page with a
  hairline, and the page in a scroll view (20 px from the top, 24 px side insets, at most 680 px wide,
  centered). Pages are built only from `apps/settings/ui.jsx`: `Stack` (sections 20 px apart),
  `Section` (13/600 header, an inset `GroupBox`, an 11 px footer), `SettingRow` (28 px of content
  with 4 px above and below, so 36 px between hairlines; the control at the trailing edge),
  `ValueRow` (value right-aligned in `textDim`), `FieldRow` (a field of a fixed width at the trailing
  edge), `Actions` (buttons under a group, trailing), `Note`/`Message`. Buttons inside rows are
  `small` (22 px); switches are `<Toggle small>` (36 × 20). Pickers between a few pictures (light/dark,
  Dock side) are thumbnail tiles with a 2 px accent ring.
- **Passwords (an isolated app):** a 220 px source list (`SearchField`, then accent glyph rows,
  `SectionTitle` captions, category rows with 18 px colored tiles and tag rows with a color dot; a 41 px
  bottom bar with the generator, settings and Lock), a 290 px item list (17/600 title, 48 px rows with a
  30 px tile, sort and "+" as toolbar buttons) and the detail pane (22/600 title, chips, the fields in
  an inset group with icon-only copy/show buttons, small secondary buttons, destructive ones in red).
  Under 960 px the columns shrink to 200 / 250 px and the detail title to 17 px. Category glyphs are
  drawn (`apps/passwords/glyphs.jsx`); a category stores the glyph's name, and old emoji values map to
  the nearest glyph. The lock screens are a 360–460 px centered column with a 22/600 title and a neutral
  method switch; sheets are 48 px title + 20 px padding and scroll when the window is too small. An
  isolated app gets the UI direction with the language (`locale.dir`), so it mirrors in RTL too.
- **Preferences panes (Terminal):** sections as sidebar rows (not a long tab bar that cannot fit),
  a header with the section and profile names and the `Done` button, and a form with a 170 px
  right-aligned label column. Measure the pane: under 480 px the labels go above their controls.
  Lists of editable items (key mappings, triggers, shortcuts) are inset `GroupBox` rows.

## Theme tokens for the shared controls

A theme gives the shared controls (`sdk/components.jsx`, `sdk/controls.jsx`, Settings' `ui.jsx`,
the shell's confirmation and power sheets) their shape through tokens next to the colors
(`sdk/theme.js`, `DEFAULT_TOKENS`). Apps don't read them: they use the controls, so every app takes
the theme's look without changes, and isolated apps receive the same tokens through the broker.

| Token | Lord Flow | Lord Classic | What it changes |
|---|---|---|---|
| `controlRadius` / `controlRadiusSmall` | 7 / 6 | 4 / 4 | `Button`, `TextInput`, `SearchField`, `Segmented`, `Stepper`, toolbar buttons, sidebar rows, pop-up buttons |
| `groupRadius` | 10 | 6 | `GroupBox`, Settings' sections and page headers, dialogs (+2) |
| `menuRadius` / `menuItemRadius` | 8 / 5 | 8 / 4 | `MenuButton` menus, pop-up lists, tooltips (`menuRadius − 2`) |
| `menuHighlight` | `'accent'` | `'subtle'` | the hovered menu item: filled with the accent and white text, or a light fill |
| `selection` | `'fill'` | `'indicator'` | the selected `SidebarItem`: an accent fill, or a light fill with a 3 px accent bar at its leading edge |
| `inputFocus` | `'ring'` | `'underline'` | the focused `TextInput`: an accent border, or a 2 px accent line along its bottom |
| `buttonBorder` | `false` | `true` | a 1 px border around neutral buttons and pop-up buttons |
| `popupBg` | `null` (by mode) | its own | the surface of menus, lists and tooltips |
| `focusRing` | `null` (the accent) | `null` (the accent) | the color of the keyboard focus ring ([Keyboard focus](#keyboard-focus)) |

Control heights, spacing and the type scale stay the same in both themes, so layouts don't move
when the user switches.

## Lord Classic

The second full appearance (0.8.3), for people used to a taskbar layout. Same apps, same SDK; only
the frame, the shell and the tokens above change. Our own design: drawn icons (`themes/classic/glyphs.jsx`),
Inter, the LordOS wallpapers and LordOS names, no assets or names from any other system.

| | |
|---|---|
| ![desktop](classic/shell-desktop-dark.png) | ![start](classic/shell-start-dark.png) |
| The desktop: the centered taskbar and a window frame | Start menu: search, pinned, recent, user and power |
| ![search](classic/shell-search-dark.png) | ![quick](classic/shell-quick-light.png) |
| Typing in the start menu searches like the system search | Quick settings, light mode |
| ![notifications](classic/shell-notifications-dark.png) | ![task view](classic/shell-taskview-dark.png) |
| Notifications and the calendar, from the clock | The Overview before Desktops (Super+Tab): live windows, smaller ([DESKTOPS.md](DESKTOPS.md)) |
| ![layouts](classic/shell-snap-layouts-dark.png) | ![preview](classic/shell-snap-preview-dark.png) |
| Snap layouts, from the maximize button | Dragging to the left edge: the preview |
| ![rtl](classic/shell-rtl-dark.png) | ![switch](classic/shell-switch-dark.png) |
| Arabic: the taskbar and the frame mirror | Settings → Appearance → Desktop style |
| ![before](classic/shell-before-start-dark.png) | ![after](classic/shell-start-light.png) |
| Before (0.8.2): the Lord Classic template | After: the start menu in light mode |
| ![files rtl](classic/files-classic-rtl.png) | ![settings rtl](classic/settings-classic-rtl.png) |
| Files in Arabic | Settings in Arabic: the selection bar on the leading edge |

- **Window frame:** a 34 px title bar with the app icon and the title (12 px) at the start and
  44 × 34 px minimize, maximize/restore and close buttons at the end (close turns red on hover);
  corners of 8 px, square when maximized or snapped; the focused window has a stronger border and
  shadow. In right-to-left languages the engine mirrors the row (buttons at the start).
- **Menus inside the window (0.8.5):** Lord Classic has no menu bar on top, so an app that declares
  menus (`useAppMenus`: Files, Text Editor, Notes, Terminal, PDF Viewer, Photos, Screenshots, Mail,
  Passwords) shows them in a 26 px strip under its title bar (File, Edit, View, Go…, then Window and
  Help, one menu open at a time; `MenuStrip` in `themes/classic/components/Window.jsx`). Apps without
  menus of their own get no strip. Help holds About <app>; the Window menu is the same in both themes
  (`windowMenuItems`, `sdk/windowmenu.js`: Minimize, Zoom, Fill, Center, Move & Resize, Move to
  <display>, Bring All to Front and the open windows), and the app menu and Force Quit come from
  `sdk/appcontrol.js` (Lord Flow shows them under the app's name and in the crown menu).
- **Taskbar:** 48 px (56 px wide on the left or right), a translucent material with the theme's
  blur (none with Transparency off). Buttons are 40 × 40 with a 6 px radius; the running app has a
  short pill below its icon, longer and in the accent color when it is in front. Tooltips after
  600 ms. The tray ends with a narrow "show desktop" strip.
- **Panels** (start menu 640 × up to 700, quick settings 360, notifications 380): 10 px radius,
  the panel material with a 1 px border, 28 px side insets in the start menu, a footer band
  (`panelFoot`) for the user and power or the battery and Settings.
- **Quick settings tiles:** 48 px high, accent fill when on, a chevron segment that opens the
  matching Settings page.
- **Shortcuts:** Super alone opens the start menu; Super+arrows snap the window in front; Super+Tab
  is the Overview; Super+A quick settings; Super+N notifications; Alt+F4 closes the window in front.

Every app in both themes, dark and light, is in [`docs/classic/`](classic/) (`<app>-classic-dark.png`,
`<app>-classic-light.png`, `<app>-default-dark.png`, `<app>-default-light.png`; 28 apps), with
right-to-left samples (`files`, `settings`, `calculator`, `notes`, `mail`: `<app>-<theme>-rtl.png`)
and the shell (`shell-*.png`; `shell-before-*.png` is the 0.8.2 template).

| Lord Classic | Lord Flow |
|---|---|
| ![](classic/settings-classic-light.png) | ![](classic/settings-default-light.png) |
| ![](classic/mail-classic-dark.png) | ![](classic/mail-default-dark.png) |
| ![](classic/calculator-classic-dark.png) | ![](classic/calculator-default-dark.png) |

## Scroll bars (0.8.3)

Every scrolling area uses the SDK's `ScrollView`, which draws an **overlay scroll bar**
(`sdk/components.jsx`, model in `sdk/scrollbar.js`):

- **Over the content, at the very edge:** a 6 px thumb, 2 px from the end edge of the view (and from
  its top and bottom), widening to 10 px and darkening under the pointer or while dragged. It never
  reserves a strip: **give the content its normal padding** (`contentStyle`, or a padded first
  child) and the bar sits in it. Content that should run to the edge of a card can bleed into the
  card's padding: `style={{ marginHorizontal: -12 }} contentStyle={{ paddingHorizontal: 12 }}`
  (Monitor → Network). At least 12 px of end padding keeps the widened bar off the text.
- **Hidden until used:** it appears while the view scrolls (wheel, touchpad, a keyboard jump through
  `scrollRef.scrollTo`, dragging the thumb) and, with *Automatically*, when the pointer comes within
  16 px of the edge; it fades out over 200 ms a second after the last scroll. Settings → Appearance
  → **Show scroll bars**: *Automatically* (default), *When scrolling* (no pointer reveal), *Always*
  (a persistent bar, for accessibility; the 0.8.1 `gutter` prop reserves an 11 px strip only in this
  mode). With **Reduce motion** it shows and hides without the fade.
- **Hit-testing:** while shown, a 14 px strip at the edge over the thumb takes the pointer, so the
  thumb can be dragged; while hidden it has `pointerEvents: 'none'` and clicks reach the content.
  The fade changes only the thumb's opacity with `lord.setStyle` (no React render per frame).
- **Direction and themes:** the bar is on the end edge (`end`), so on the left in right-to-left
  languages. The theme styles it with the optional tokens `scrollThumb`, `scrollThumbHover`,
  `scrollRadius` and `scrollThumbBorder` (a pill in Lord Flow; the Lord Classic theme's is squarer and
  solid with a hairline). `ThemeProvider` passes the setting (`scrollBars`) and `reduceMotion` with
  the tokens, so isolated apps follow them too.
- **New content starts at the top (0.8.8.3):** a view that shows one page out of several (a Store
  category, a Settings section, a folder, search results) passes `resetKey` (the page's id); when it
  changes, the view goes back to the top instead of keeping the last page's position, which could
  leave a short page scrolled past its end. Views whose content just updates (a log, a refreshing
  list) leave it out and keep their position. The Store also returns to where a list was when you
  go back from an app's page.
- **Keyboard and sideways (0.8.8.8):** a `ScrollView` whose content doesn't fit is a Tab stop and scrolls with
  the keys ([Keyboard focus](#keyboard-focus)); with `horizontal` it scrolls along its row, its bar along the bottom
  edge.
- The Terminal's history bar (drawn in C, `term_paint.c`) uses the same size and inset and shows
  only while you look at the history.

Tests: `tests/scrollview_reset_test.mjs` (`resetKey`), `tests/scrollbar_model_test.mjs` (geometry, thumb hit-test and drag, show/hide per mode,
fade timing, reduce motion) and `test_overlay_scrollbar` in `build/engine-test` (painted over the
content, no strip reserved, never over the text, the strip takes the pointer only while shown,
the faded opacity, RTL). Screenshots: `scrollbars-network-{light,dark}-{hidden,scrolling}.png`,
`scrollbars-settings-{light,dark}.png`.

![Monitor → Network while scrolling](scrollbars-network-light-scrolling.png)

## Keyboard focus

Every shared control can be reached and used without a mouse (`sdk/focus.js`, the pure model in
`sdk/focusmodel.js`). **Tab** and **⇧Tab** go round the focusable controls of the window in front in tree order,
skipping the hidden (`display: 'none'`) and the disabled ones, and wrap at the ends; while a sheet or prompt of the
shell is open (the modal, system, overlay or capture layer) they go round its controls instead, and inside a View
with the `modal` prop while it is shown. Ctrl+Tab and ⌥Tab stay the apps' and the shell's.

- **Which controls:** `Button`, `ToolbarButton` (and so `MenuButton`), `Toggle`, `Segmented`, `Slider`,
  `Stepper`, `TextInput`, `TextArea`, Settings' pop-up buttons (`PopUp`), `Dropdown`, `Checkbox`, `RadioGroup`,
  `Tabs`, `List`, `Table`, and a `ScrollView` whose content doesn't fit.
  Each one takes `focusable={false}` to leave the order; a disabled one is never in it. Files' icon, list and column
  views pass `focusable={false}`: there the arrows move the selection. Any `View` becomes a stop with `focusable`,
  and `onKeyDown(e)` (return `true` when it used the key) gives it keys while it is focused.
- **What the keys do:** Space and Enter press a button (its `onClick`, as a click would) or flip a toggle; ← →
  and ↓ ↑ move a slider (a twentieth of its range, or `step`) and a stepper, Page Up and Page Down a tenth (ten
  steps), Home and End to the ends; ← → choose a `Segmented` segment; ↓, Space or Enter open a `MenuButton` or a
  pop-up button, ↓ ↑ go through its items, Enter chooses and Escape or Tab close it. A focused scroll view (or one
  with a control inside it focused) scrolls with ↓ ↑, Page Up and Page Down, Space and ⇧Space, Home and End; a
  `horizontal` one with ← → too, and with Shift and the wheel or a sideways swipe. A control Tab moves to inside a
  scroll view comes into sight. A key a control used reaches no `useKeyboard` handler: Space on a toolbar button in
  Files presses it and opens no Quick Look.
- **Right to left:** the order is the tree's, so it follows the layout's start and end. ← and → swap for sliders,
  steppers, segmented controls and horizontal scroll views (a slider runs from the start edge, so its minimum is on
  the right in RTL); ↑ and ↓ never swap.
- **The ring** (as `:focus-visible`): a 2 px line in the theme's `focusRing` token (the accent without one), 1 px
  outside the control and following its corners (inside the box of a scroll view, which clips). It appears only after
  the keyboard moved the focus; a press of the pointer ends the focus and hides it, and the next Tab goes on from
  where it pressed. Text fields keep their own accent border and get no ring. A confirmation dialog in front hides
  it and keeps Escape, Enter and Tab for its two buttons (`sdk/dialogkeys.js`);
  the ring comes back when it closes.
- **Text fields:** a `TextInput` reached with Tab has its text selected (the first key replaces it), a `TextArea`
  keeps its caret; both take every key while they have them and let Tab go to the next control, unless they are in
  a `FormFields` (Tab then goes round its fields, as before) or the field's own `onKeyDown` used Tab (completing an
  address). An app's own text field (`takeTextFocus`: Notes, Mail's body) keeps Tab.
- **Apps that want Tab** call `e.preventDefault()` in their key handler: the Terminal (the shell completes), the Text
  Editor (it indents), a virtual machine's screen and console, the remote desktop, the Databases completion list, the
  Imager's own focus and the Overview do. Tab moves the focus only after every listener had the key.
- **Lists and tables** (`List`, `Table` in `sdk/lists.jsx`, 0.8.8.8): one Tab stop, and a press on a row gives it
  the keys too, without the ring (`focusOnPress`). ↑ ↓, Home, End, Page Up and Page Down choose a row; with
  `multiple`, Shift and a move or a press extend a range from the anchor, Ctrl (⌘) and a press toggle a row, Ctrl and
  an arrow move only the outlined lead row and Ctrl+Space toggles it, Ctrl+A selects all. Typing a name goes to the
  next row that starts with it; Enter and a double click activate. The row the keys reach comes into sight.
- **Choice controls** (`sdk/controls.jsx`): a `Dropdown` opens with Space, Enter, ↓ or ↑ on the current choice, then
  ↓ ↑, Home and End move, Enter or Space chooses, Escape closes it and letters jump (also while it is closed).
  `Tabs` and a `RadioGroup` are one stop whose arrows choose (← → swap in RTL; they go round and skip disabled ones).
  A `Checkbox` flips with Space. A `Popover` is a focus scope: opened from the keyboard the focus moves to its first
  control and Tab stays inside it; Escape closes the newest popup only (a `Dropdown` open inside a popover first) and
  the focus goes back to the control that opened it. A `Tooltip` shows after a pause on hover and on keyboard focus
  (a toolbar button's too), below its control or above it, never over the pointer. `ProgressBar` is determinate
  (0–1, from the start edge) or, with no value, a sliding band (still with reduced motion). The wheel moves a slider
  or a stepper by a step; a touchpad swipe only while it has the focus, so a page scrolling past it keeps scrolling.

Tests: `tests/focus_model_test.mjs` (tree order, skipping, a modal node, focus-visible, the keys of sliders,
steppers, segmented controls, menus and scroll views, RTL) and `tests/focus_scene_test.mjs` (the controls in a
window through the runtime and the server's own events: the ring, Space and Enter, the arrows, a menu, a text field
in the order, a press that ends the focus, a prevented Tab, a dialog in front); `tests/list_model_test.mjs` (ranges,
toggles, page moves, type-to-find), `tests/control_model_test.mjs` (Tabs, radio groups and Dropdown keys, the wheel,
tooltips, progress) and `focus_scene_test.mjs list_scene.mjs` (the List, the Table, the choice controls and a
popover's focus through the runtime).

## Robustness checklist

Check every pass on the VM with:

- **light and dark** mode;
- **RTL** (Arabic test pack `docs/packs/lordos-lang-ar.json`). The engine mirrors rows. Content
  that is itself left to right keeps `direction: 'ltr'` (calculator keypad, code, sliders, page
  geometry), and user text follows its own direction with `textDirection(text)` (a Spanish note in
  an Arabic UI still reads left to right);
- the **minimum window size** (`minSize` in the manifest) and a **large/maximized** window;
- no overlapping or clipped controls, and no toolbar that wraps. Text that can be long uses
  `whiteSpace: 'nowrap'` and `textOverflow: 'ellipsis'` inside a `flex: 1` box (see "Engine notes").

## Engine notes

- **Long text:** truncate it, don't let it be cut at the edge. A file name, a song title or a
  subject in a row: `whiteSpace: 'nowrap', textOverflow: 'ellipsis'` inside a `flex: 1` (or
  `minWidth: 0`) box. A preview or a description: `numberOfLines: 2` (the last line ends in "…").
  Text that has a fixed height and `textOverflow: 'ellipsis'` keeps the lines that fit. The cut
  never splits a ligature, a mark or an emoji sequence, and the "…" goes where the text ends: on the
  left in Arabic and Hebrew (screenshots: `engine-ellipsis-*.png`).
- **Hairlines:** a separator that belongs to a box is a border of that box:
  `borderBottomWidth: 1, borderBottomColor: t.separator` (or `borderTop…`, `borderStart…`/`borderEnd…`,
  which follow RTL). Remember that a border is inside the box: add 1 px to a fixed height to keep the
  content where it was. `Hairline` (an absolute 1 px view) is still the tool for a line inset from
  the edges or over something else.
- **Corners:** `borderTopLeftRadius`… or the logical `borderTopStartRadius`… for shapes rounded on one
  side (a segmented control's ends, a bubble).
- **Logical edges:** `marginInlineStart`/`End`, `paddingInlineStart`/`End` and `insetInlineStart`/`End`
  follow the node's direction, so `[direction() === 'rtl' ? 'right' : 'left']` is no longer needed
  (`left`/`right` and `margin*Left`/`Right` stay physical, as in CSS).
- **Scale and rotate:** `transform: [{ scale: pressed ? 0.96 : 1 }]` for a press effect,
  `transform: 'rotate(…deg)'` for a spinner; layout doesn't change and clicks follow what is painted.
  lord-server paints the subtree once into a cached raster and composites it, so animating the scale
  is cheap (the Dock's minimize effect does it on a whole window: `engine-dock-minimize.png`).
- **`flex: 0`** is React Native's: the box keeps its `width`/`height` and doesn't grow or shrink (it
  used to mean a zero basis, which collapsed a box with a `width`). `flex: 1` and a `width` on the same
  box still mean "grow from 0" (the `width` is ignored): pass one or the other.

## Pending (engine)

- No vector icon set: icons are font glyphs (see above).
