# Notes (sticky notes)

A native LordOS app that comes preinstalled: `apps/notes`, in the Dock by default. They are
markdown notes that are read with formatting and a table of contents, accept images, attachments
and emoji, and can be placed on the desktop as sticky notes (widgets) that move and resize. They
save themselves in the home folder.

## Screenshots (VM 118, real lord-server)

| | |
|---|---|
| ![editing](notes-editing.png) | ![reading](notes-reading.png) |
| Editing: markdown with the headings highlighted; lists and checkboxes continue on their own with Enter | Reading: headings, bold, italic, code, checkboxes ticked with a click, numbered list, quote, table and table of contents |
| ![outline](notes-index-image.png) | ![emoji](notes-emoji.png) |
| The table of contents jumps to the heading; the image is part of the note | Emoji picker with search in Spanish or English (also `:name` while typing) |
| ![desktop](notes-desktop-image.png) | ![pinned](notes-pinned.png) |
| A note on the desktop, with an attachment, checkboxes and an image | Pinned to the front: it stays above the windows |
| ![classic](notes-classic-theme.png) | |
| Lord Classic theme: a note pinned in front of the Terminal | |

## Where they are stored

Each note is a folder in `~/Notes/<note>/`:

```
~/Notes/note-20261016-135509/
├── nota.md          the note
├── image-1.png      pasted from the clipboard
├── m1-first-frame.png   inserted with «Image»
└── informe.txt      attached with «Attach»
```

Before 0.7.0 the folder was `~/Notas`. On the first run Notes moves it to `~/Notes` (a rename
in the home folder through `lord.fs`). If both folders exist, Notes uses `~/Notes` and leaves
`~/Notas` untouched; Search also falls back to `~/Notas` while `~/Notes` doesn't exist yet. If
there is no Trash, a deleted note goes to `~/Notes/.trash`.

`nota.md` is plain markdown with a small YAML header: it opens with any editor. Since 0.8.9.6.2 Notes sees changes
made by other programs at once: it watches `~/Notes` with `lord.fs.watch` (read-only inotify, coalesced; a note folder
made or deleted), the folders of up to 4 notes on the desktop, and for a moment a folder just made without its
`nota.md` (at most 3), and reads the folder again on each change, with no window open and no timer (a note an
assistant writes with `desktop: true` is on the desktop within a second). The window reads it again when it opens and
when it gets the focus (any other edit by hand, and a cloud or network `~/Notes`, which `lord.fs.watch` refuses). A
note with a change not saved yet, or whose save failed, keeps what is in memory (`apps/notes/notesmodel.js`
`mergeScan`); a change in a note's folder in the second after Notes saved it is its own save and is not read again.

```markdown
---
title: Compras del finde
color: green            # yellow | pink | green | blue | purple | gray
x: 480                  # position and size on the desktop
y: 187
w: 387
h: 420
desktop: true           # shown on the desktop
pinned: false           # pinned above the windows
created: "2026-10-16T13:55:09.073Z"
updated: "2026-10-16T13:56:30.386Z"
---
# Compras del finde
- [x] yerba
```

- **Saving.** It is automatic, about 600 ms after the last change, with `fs.write` (atomic).
  Pending changes are also written when the window closes.
- **Title.** It is the first line with text, without the markdown marks.
- **Deleting.** The note's folder goes to the Trash (`fs.trash`) and can be restored from Files.

## Window

On the left is the list of notes, with search in the title and the text, the color and the date, and
the **+** button beside the search field that makes a new note (its tooltip says "New Note"; until 0.8.9.5 it was a
✎, which read as "edit", GitHub #238). On the right is the open note, with these buttons:

- **Read / Edit:** ❖E switches between them; a double click while reading starts editing.
- **Colors.**
- **⊕ Emoji / Image / Attachment:** the emoji picker, or a file from the system file dialog (FILES.md, "The system
  file dialog") copied into the note. Inserting is editing: from the reading view these rows (and a drop from Files)
  open the editor first and the text goes in at its caret (GitHub #238).
- **On the desktop.**
- **Duplicate:** also copies the attachments.
- **Delete.**

Files dragged from Files (0.8.8.8) are copied into the open note, or into a new note when none is open:
PNG and JPEG files become images and the others attachments, as «Image» and «Attach» do; folders are
skipped. While the drag is over the window a dashed outline says what letting go will do (`useFilesDrop`
and `FilesDropHint`, `sdk/filedrop.js`).

The File menu also has «New note» (❖N), «Import…» and «Show in Files». **Import…** (0.8.9.6.2) takes `.md`,
`.markdown` and `.txt` files from the system file dialog, one or several: each becomes a new note in a new folder (an
import never writes over a note). The file is untrusted text: a Notes header in it gives only its title and colour,
another program's front matter stays in the text, a file over 8 MB, with a NUL byte, a link or a folder is not read,
and nothing it names is copied. Markdown and text files dropped on the list are imported too (and on the window when no
note is open); other files, and files dropped on an open note, are its attachments.

The toolbar's **checklist** button (a ring with a tick, beside the colours) makes the caret's line (or the selected
lines) a checklist item, `- [ ] `, or plain text again, as ❖⇧9 does; from the reading view the editor opens first. In
the reading view (the window and the desktop notes) a checklist item is a round check (`<Markdown taskStyle="round">`):
a 21 px ring when open, filled with the note colour's tint and a white ✓ when done, in a 28 px target on the item's
first line; the done text stays readable (not dimmed, not struck through). The editor itself shows the markdown
(`- [ ]`), so the circles are ticked in the reading view.

### Attachments and links (GitHub #238)

A click on an attachment opens that file alone: a text file in Text Editor, anything else in its default app (Photos
shows only that picture, not the note's other files), and never Files on the note's folder (which showed `nota.md` and
every other attachment). When no app opens it, nothing opens. Only PNG and JPEG are drawn in the note (what
lord-server's own decoder reads, `server/src/ui/stb.c`); GIF, WebP, BMP, HEIC, HEIF, AVIF and TIFF are attachments that
open in Photos. An attachment's name is kept safe for the folder and for the link (`apps/notes/links.js`: no slashes,
control or direction-override characters; spaces and parentheses percent-encoded in the link), and a folder or a link
is not attached.

A note is markdown anyone can write by hand, paste or sync, so its links and images are untrusted. Notes opens or
draws only the note's own files, by plain name, that are real files (not links) other than `nota.md`: `file://`,
`javascript:`, `data:` and any other scheme, absolute paths, `~`, `..` (encoded or not), a link planted in the folder,
and remote images are neither opened nor fetched (a remote image shows its `[alt]`; a web address is shown, not
opened). A note folder or a `nota.md` that is a link, and a `nota.md` larger than 8 MB, are not loaded; a header
edited by hand is read within sane bounds (size 160–4000 px, a known colour). Checked by
`server/tests/notes_store_test.mjs` over a real temporary home and by `tests/app_scene_headless.sh notes_scene`.

## On the desktop

Each note marked «On the desktop» is a sticky note between the wallpaper and the windows.

- Hovering shows the bar with the colors, ✎/👁 (edit or read), 😀 (the emoji picker, drawn in the popover layer
  beside the note in the desktop's colours: inside a small note it was cut off and unreadable, GitHub #238), 📌 (pin to the front),
  ⤢ (open it in the window) and ✕ (remove it from the desktop: the note isn't deleted).
- It is dragged by the bar and resized from the bottom-right corner.
- The position, size and color are stored in the note, so after a reboot it comes back in its
  place (tested).
- It is edited right there: it takes the text focus (the SDK's `takeTextFocus`), so keys don't
  go to the frontmost window. Esc goes back to reading.
- **Add widget → Sticky note** (0.8.9.6.2, every theme with the catalog) offers «New Note» or one of the notes already
  made: a searchable list (title and text, no accents or case), most recently updated first, with each note's colour
  and date and «on the desktop» on those already there. A note picked goes where the widget is being added, moved
  inside that display's work area, with its text and size (a generic hook: `registerWidgetKind({ …, Chooser })`,
  `sdk/widgetpicker.jsx`).
- «On desktop» from the window puts a note whose saved place is off every display (a monitor gone) on the window's
  display; a note whose bar can be seen keeps its place.

## Editing

| Shortcut | What it does |
|---|---|
| ❖B / ❖I | **bold** / *italic* (on the selection, or leaves the marks ready) |
| ❖K | link `[text](url)` |
| ❖1 / ❖2 / ❖3 | heading level 1, 2 or 3 (again removes it) |
| ❖⇧7 / ❖⇧8 / ❖⇧9 | numbered list / bullets / checkboxes `- [ ]` |
| ❖⇧C | code block |
| Enter in a list | continues the list (next number, empty checkbox); on an empty item it ends it |
| Tab / ⇧Tab in a list | indent / outdent |
| ❖Z / ❖⇧Z | undo / redo |
| ❖C / ❖X / ❖V | copy / cut / paste. If what was copied is an image (from any app, Linux ones too), it is saved as `image-N.png` in the note and inserted |
| `:` + name | emoji suggestions (`:cafe` → ☕); Enter or Tab picks |

The editor is our own (`apps/notes/NoteEditor.jsx`): text in a monospace font with word wrap,
mouse selection (double click: word; triple click: line), input methods (Japanese, Chinese,
Korean) and highlighted headings.

Since 0.8.9.6.2 a row is measured the way it is drawn (`apps/notes/wrapmodel.js`): the whole row's string with
`lord.measureText` (regular, or bold for headings), so CJK, emoji, accents and a tab (drawn as four spaces; a CR from CRLF
line ends is not drawn) never make a row wider than the editor or the caret drift off the letters. Typing is fluid
on long notes: the editor keeps the text itself; only the line being edited is wrapped again; only the rows in sight are
drawn, each a memoized row; the text goes into the note's store and is saved 500 ms after typing stops (and when the
editor closes), so the list, the other views and the desktop widgets no longer redraw on every key; the save runs on a
worker (`lord.fs.writeAsync`, `server/src/fsasync.c`: a temporary file, fsync, rename; one at a time per note, in
order; a failed save is kept as not saved and tried again in 5 s; closing or logging out writes at once). On a 300-line
note in the headless scene (`tests/app_scene_headless.sh notes_edit_scene`) the longest main-thread run while typing
went from 41–68 ms to 9–12 ms.

## Markdown (the SDK's `<Markdown>`)

`sdk/markdown.jsx` is reusable by any app: `<Markdown source base width onLink
onToggleTask onHeadingRef emoji resolveImage />` (`resolveImage(target)` returns the file to draw or null; Notes allows
only the note's own PNG and JPEG files), plus `markdownHeadings(text)` to build a table of contents
and `toggleTask(text, line)`.

- **Blocks:** headings, paragraphs, quotes (nested), lists with levels, numbered lists,
  checkboxes (`- [ ]` / `- [x]`, ticked with a click, which changes the `.md`), code blocks,
  tables with alignment, and separators.
- **Inline:** bold, italic, strikethrough, `code`, links, bare URLs, local images
  `![](image.png)` (relative to the note's folder, at their real size up to the available
  width) and `:emoji:`.
- **How mixed-style text is built.** The LordOS engine has no runs with different styles inside
  a single `<Text>`. So a paragraph is a row with `flexWrap: 'wrap'` and one `<Text>` per word
  (the space is attached to the word): it breaks lines between words and each word carries its
  style.
- **Strikethrough.** A 1 px line drawn across each word (the combining character U+0336 used before
  is missing from most fonts and drew boxes).

## Desktop widgets (`sdk/widgets.js`)

It is a generic layer, not just for notes:

- An app registers a type with `registerWidgetType('note', Component)` and publishes its widgets
  with `setWidgets('note', [{ id, x, y, w, h, pinned }])`.
- Both themes draw `<DesktopWidgets layer="below" />` between the wallpaper and the windows, and
  `<DesktopWidgets layer="above" />` on top, for the pinned ones.
- Notes are loaded when the session starts (the app's module is evaluated with the theme), so
  the desktop ones appear without opening the app.

## New server APIs

- `lord.clipboard.imageData()` (`js_term.c`): the copied PNG as an `ArrayBuffer`, or `null`. The
  app saves it with `lord.fs.write`, under its usual rules.
- `lord.image.size(path)` (`js_sys.c`): the width and height of an image in the home folder, to
  show it at its size.

## Pending

- The text area is always `ltr`, as in the editor; the reading view follows the note's own direction (checked in
  Arabic in the headless scene, 0.8.9.5).
- The editor's monospaced font differs from the reading view's (GitHub #238 asks to merge the two modes).
- Seeing hand-made changes to `nota.md` without reopening the session.
- Timed reminders (⏰), shared notes, sync.
- In the editor emoji show in black and white (mono font); while reading, in color.
