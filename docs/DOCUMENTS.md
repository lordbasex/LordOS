# Documents (M9)

PDFs, archives, fonts and emoji in LordOS. This document covers:

- the **PDF Viewer**;
- in **Files**, archives and **Quick Preview**;
- **Fonts**;
- **Emoji & Symbols**;
- the sandboxed helper **lord-doc**, which does the heavy lifting for all of the above.

Printing and scanning are in `PRINTING.md`.

## Screenshots (VM 116, real lord-server)

| | |
|---|---|
| ![outline](pdf-outline.png) | ![search](pdf-search.png) |
| PDF Viewer: the document's outline (bookmarks) on the left | Search with highlighting: the current result in orange and the match count |
| ![annotations](pdf-annotations.png) | ![note](pdf-note.png) |
| Highlight and freehand stroke, drawn by Poppler on the page and on the thumbnail | Note: the icon stays on the page and is saved in the copy |
| ![form](pdf-form.png) | ![dark](pdf-dark.png) |
| Form (text fields of a CUPS PDF) filled in | Dark page: luminance inverted, keeping the colors |
| ![rotate](pdf-rotate.png) | ![quick look](quick-preview-pdf.png) |
| Rotated a quarter turn; the fields stay in place | Quick Preview of a PDF: the first page and the page count |
| ![font](quick-preview-font.png) | ![zip](quick-preview-zip.png) |
| Quick Preview of a `.ttf` font | Quick Preview of a `.zip`: count, size and what's inside |
| ![menu](archives-menu.png) | ![inside](archives-browse.png) |
| "Extract here" and "Compress" in the context menu | A `.zip` opened as a folder |
| ![extract](archives-extract.png) | ![compress](archives-compress.png) |
| A zip with `../` in the names: the safe entries are extracted, 2 entries are skipped for safety | "Compress" a folder: "Trip photos 2.zip" |
| ![fonts](fonts-sample.png) | ![characters](fonts-characters.png) |
| Fonts: sample with text and sizes of your choice | Character table by Unicode block |
| ![file](fonts-file.png) | ![installed](fonts-installed.png) |
| A `.ttf` opened from Files, not installed | Installed for the user (`~/.local/share/fonts`), with "Remove" |
| ![picker](emoji-picker.png) | ![editor](emoji-in-editor.png) |
| ❖. in the Editor: search "heart", skin tones | 😍 and 📕 typed into the Editor through the input method path |
| ![emoji](emoji-picker-app.png) | ![symbols](characters-app.png) |
| Emoji & Symbols: a category with skin tone | Unicode symbols by block (arrows) |
| ![rtl](pdf-rtl.png) | |
| In Arabic: the interface mirrored (sidebar on the right) and the page in its own direction; the field is filled in the same way | |

## PDF Viewer (`apps/pdf`)

In the style of a classic document viewer:

- **View:**
  - continuous scrolling or one page at a time;
  - sidebar with thumbnails or with the PDF's outline (collapsible);
  - zoom (−/+, fit width, whole page, actual size), with ❖ and the wheel;
  - rotate ↻;
  - dark page: inverts the luminance and keeps the hue (◐).
- **Search:** the field in the toolbar (❖F). It highlights everything, marks the current result in
  orange and jumps to it (Enter and ⇧Enter, ❖G). Results arrive page by page: the first ones show
  up right away, even if the document has hundreds of pages.
- **Text:** dragging selects (letter by letter, a word with double click) and ❖C copies. ❖A
  selects the page.
- **Go to a page:** with a click on "Page N of M" or with ❖⌥G.
- **Links:** internal ones go to the page; external ones are copied to the clipboard (LordOS doesn't
  have its own browser yet).
- **Annotations:**
  - highlight ▬ (five colors);
  - note 🗨 (a text at a point);
  - freehand ✎.

  ❖Z undoes the last annotation. Everything is saved **as a copy**, "name (annotated).pdf" (the suffix in the interface language) next to
  the original, with ❖S or File → Save a copy. The original is never touched.
- **Forms:** text fields, checkboxes, radio buttons and lists are filled in place, and saved with
  the copy.
- **Print** (❖P or ⎙): opens the system dialog with the document as it is on screen, including
  forms and annotations.
- **Password-protected PDFs:** the password is asked for before opening.
- **If the helper crashes** with a damaged PDF, the viewer says so and offers to reopen it. The
  desktop carries on as usual.

How it draws:

- Pages are requested from lord-doc only for what's visible, plus the neighbors at lower priority.
- A request that's no longer needed (the page left the view) is canceled before it starts.
- While a page arrives at the new zoom, whatever version of that page exists is shown, scaled:
  there's never a gap.
- **Cache** (`apps/pdf/cache.js`): 48 MB of pages and 8 MB of thumbnails per document. On top of
  that there's a 160 MB cap for all the images lord-server keeps (`doc.c`).

## Archives in Files

- **Looking inside:** double-clicking a `.zip`, `.tar`, `.tar.gz/.xz/.zst/.bz2`, `.tgz`, `.7z`,
  `.gz`… opens it as a folder (`{ type: 'archive', path, inner }` in `apps/files/archive.js`).
  - The folders inside are browsed just like real ones.
  - Opening or previewing with Space a file from inside extracts **only that one** to
    `~/.cache/lordos/archives/<id>/` and opens it with its app.
  - Inside the archive you can't delete, rename or paste.
  - "Extract N items" takes the selection out to a new folder next to the archive.
- **Extract here:**
  - creates a new folder named after the archive without the extension; if it already exists,
    "name 2";
  - shows the progress in the status bar;
  - at the end it says how many files came out and how many were skipped for safety.
- **Compress:** the selection goes to a `.zip` alongside: "name.zip" if it's a single item, or
  "Archive.zip" (translated, `docs.archiveName`). It handles recursive folders, symbolic links as links and UTF-8 names.
- **Formats read** by libarchive: zip, tar (gz, xz, zst, bz2, lzma, lz), 7z, cpio, iso, ar, rar
  (list and extract only, as libarchive allows) and standalone `.gz/.xz/.zst/.bz2`. It only writes
  zip.
- **Encrypted files:** they're listed with their padlock; extracting them isn't supported yet
  (they're skipped with the reason "encrypted").

### Extracting without leaving the folder

A malicious archive can't write outside the destination folder. There are three layers:

1. **The names** (`server/src/doc/arcpath.c`, tested separately):
   - absolute paths, any `..` component, control characters, components longer than 255 bytes and
     more than 64 levels are rejected;
   - a **symbolic link** is only created if its target is relative, has `..` only at the start and
     doesn't climb above the root. So even if a component is another link from the same archive,
     it doesn't get out either;
   - **hard links** can only point to another entry inside;
   - no devices, FIFOs or sockets are created;
   - permissions are left without setuid, setgid or sticky.
2. **The disk** (`h_arc.c`):
   - each folder is opened component by component from the destination fd, with `O_NOFOLLOW`: a
     link can't serve as a bridge;
   - files are created with `O_EXCL` and never overwrite an existing one;
   - there are caps: 500,000 entries, 64 GB written and 128 MB free on the disk, against "bombs".
3. **The confinement:** lord-doc runs under Landlock with write access **only** below the
   destination fd (`rw_dir_fds`), with no network and with seccomp.

Tested in `tests/doc_evil.sh` with the archives from `tests/make_evil_archives.py`:

- zip slip (`../`, `a/../../`);
- absolute path (`/tmp/…`);
- link to `..` and to `/etc` followed by a file "through" the link;
- a chain of links that together get out;
- hard link to `/etc/passwd`;
- device, FIFO and setuid;
- repeated file (doesn't overwrite);
- names in several scripts;
- garbage with a `.zip` extension;
- standalone `.gz`.

Result: nothing outside the destination, in 31 of 31 checks (with the confinement and the
formats).

## Quick Preview (Space)

`apps/files/quicklook.jsx`. What it shows depending on the file type:

| File | What it shows | Who makes it |
|---|---|---|
| PNG/JPEG image | the image | stb_image in lord-server, as before |
| PDF | the first page and the page count | lord-doc's utility session, already warm |
| text and code | the first lines | — |
| font (`.ttf/.otf/.ttc`) | a sample in several sizes | FreeType in lord-doc |
| archive | file count, size and the first entries | — |
| audio and video | a frame or the cover through `media.poster` (`MEDIA.md`); without lord-media, the icon | lord-media |
| file inside an archive | the same, after extracting only that one to the cache | lord-doc |

The arrows go to the next one; Space or Esc close it.

## Fonts (`apps/fonts`)

- **Collections:** all, the user's, monospaced and color, with search by family.
- **Sample** with whatever text you type and a size from 8 to 120 px, and the alphabet in four
  sizes. Each style of the family is chosen at the top.
- **Characters:** the table by Unicode block, only for the blocks the font has. A click shows the
  character large with its U+XXXX, and copies it.
- **Information:** family, style, PostScript name, glyphs, characters, type, file and size.
- **Install:** when opening a `.ttf/.otf/.ttc` from Files, "Install" copies it to
  `~/.local/share/fonts`. If that family and style are already installed, it says so instead of
  copying.
- **Remove:** only the user's; they go to the Trash.

The samples and the table are drawn by lord-doc with FreeType and cairo. The catalog is the usual
one, `lord.fonts.list`, which now also carries `path` and `index`. New fonts aren't read in
lord-server until they're installed.

## Emoji & Symbols

- **The picker** (`sdk/emoji.jsx`, mounted by `startShell` in every theme):
  - it opens with **❖.** or Ctrl+❖Space, below the cursor of the focused field, or in the middle
    of the screen;
  - it **searches** by name and keywords in Spanish, English and Portuguese at once (CLDR, ignoring
    accents): "corazon", "heart" and "coração" find the same thing;
  - it has **categories** (Tab moves from one to the next), **recents** and **skin tone** (both
    saved in the settings);
  - the arrows choose and Enter types.
- **How it types:** through the **same path as the input method**. The text arrives as a key that
  types that text, just like `ime_commit` in `js.c`, so it works in `TextInput`s, the Editor, Notes
  and the Terminal:
  - in a focused **Linux app**, it pastes it through the clipboard (`lord.wayland.paste`), like the
    clipboard panel;
  - with no focused field, it copies it and says so.
- **The "Emoji & Symbols" app** (`apps/characters`) is the same in a window, plus the **Unicode
  symbols by block**: arrows, math, currencies, boxes, braille, Greek… A click chooses and "Copy"
  puts it on the clipboard.
- **The data:**
  - `sdk/emojidata.js`: 1,898 emoji by category, without text, and the 323 that accept a tone;
  - `assets/locales/{es,en,pt}/emojikw.json`: the names and the keywords.

  They're generated by `tools/gen-emoji.mjs` from Unicode emoji-test 15.1 and CLDR 45 (Unicode
  License v3).

## lord-doc: the sandboxed helper

```
lord-server (no Poppler or libarchive)
  └── doc.c: its own thread with poll() over the helpers; priority queue; deadlines
        ├── lord-doc  "pdf" session    one open PDF (Viewer, print preview)
        ├── lord-doc  "arc" session    a listing, an extraction or a compression
        └── lord-doc  "util" session   long-lived: fonts, PDFs to print, thumbnails, images
```

- **Binary:** `server/build/lord-doc` (`src/doc/helper.c`, `h_pdf.c`, `h_arc.c`, `h_font.c`,
  `h_make.c`).
  - It links Poppler (glib interface) and libarchive, and Poppler brings cairo, FreeType and
    fontconfig.
  - lord-server links **none** of those libraries.
  - If the `-dev` packages are missing, `make` builds everything else and the documents say the
    helper isn't there.
- **How it's launched:**
  - `doc.c` launches it with a **fixed path**: `lord-server`'s own folder + `/lord-doc`;
  - it must be owned by root or by the same owner as lord-server, and not writable by others;
  - with no arguments and a minimal environment: HOME, `LANG=C.UTF-8` and the LordOS fonts folder;
  - the channel is a SOCK_SEQPACKET `socketpair` on fd 3.
- **Files:** the helper **opens nothing by path**. lord-server validates the path (home folder or
  the user's mounts; for fonts, also `/usr/share/fonts`) and passes it the fd with SCM_RIGHTS:
  - for reading, the document;
  - for extracting, the fd of the destination folder (created by lord-server, new except in the
    cache);
  - for saving a copy or a `.zip`, a new file opened with `O_EXCL`.
- **The confinement** (`sandbox_apply_helper`, the shared module `server/src/sec/sandbox.c`),
  applied as soon as it receives those fds and before reading a byte of the document:
  - **Landlock:**
    - read-only `/usr`, `/etc/fonts`, the fontconfig cache and the user's fonts;
    - write access only below the destination folder (`rw_dir_fds`), and read access to the
      folders to compress (`ro_dir_fds`);
    - no TCP connections.
  - **seccomp:**
    - no `socket`, `connect`, `bind`, `execve` or `ptrace`;
    - `clone` only for threads;
    - the syscalls are in a per-architecture list (x86-64 and ARM64).
  - **Also:** no capabilities, `no_new_privs`, its own cgroup `lordos-doc.scope` (768 MB/1 GB),
    1.5 GB `RLIMIT_AS`, `PR_SET_PDEATHSIG` and no core dumps.
  - **Tested:** `build/doc-test … selftest` and `selftest-arc` try what's forbidden. They can't
    open `~/.ssh` or `~/.bashrc`, write to `/tmp`, open sockets (neither network nor unix, so no
    D-Bus, CUPS or Wayland) or fork. Fonts can be read.
- **What comes back:**
  - **JSON:** lord-server validates it with the strict reader in `sec/json.c` before passing it to
    JS.
  - **Images:** premultiplied ARGB32, the engine's format, in a memfd. lord-server requires the
    `F_SEAL_SHRINK` seal before mapping it, so a cheating helper can't cause SIGBUS by shrinking
    it.
  - **Built PDFs:** in a sealed memfd (a "blob").
- **If something fails:**
  - the compositor **never waits**: results are read with `lord.doc.poll()` only while something
    is pending;
  - each request has a deadline (30 s for a page, 60 s to open; search, extract and compress are
    cut off after 60 s without progress). Once it expires, the helper is killed;
  - if it crashes, its requests end with `crash`;
  - a malicious PDF or archive can only bring down its own helper.

### API

`lord.doc` (`server/src/ui/js_doc.c`) and in the SDK:

| Module | What's there |
|---|---|
| `sdk/doc.js` | `docAvailable()`, `<DocImage image>`, `releaseImage`, `fontInfo`, `fontSample`, `fontGrid`, `decodeImage`, `convertImage`, `makeTextPdf`, `makeImagePdf`, `pdfThumb`, `saveBlob`, `releaseBlob`, `warmDocs()` |
| `sdk/pdf.js` | `pdf.open(path \| { blob })` → `render`, `select`, `find`, `outline`, `pageInfo`, `setField`, `annot`, `undo`, `save`, `blob`, `sizes`, `close`; `pdf.thumb` |
| `sdk/archive.js` | `archive.list`, `extract(path, folder, { only, existing, onProgress })`, `create([paths], dest.zip)`, `tree`, `isArchive`, `stem` |
| `sdk/emoji.jsx` | `emoji.groups/search/name/withTone/insert/open`, `EmojiGrid`, `EmojiToneBar` |

## Performance (measured on VM 116: Xeon Silver 4108 at 1.8 GHz, 4 cores, no GPU)

| What | Time |
|---|---|
| Launch a lord-doc and confine it | 32 ms |
| Open a 224-page PDF (1.4 MB) with the sizes of all pages | 210 ms (the viewer first asks only for the first one and the rest afterwards, without holding up the first draw) |
| First page in the viewer, from the double click (135 %, 1070 px wide) | 437–479 ms |
| Page drawn by Poppler, with the document already open | median 20 ms, between 0.5 and 155 ms depending on the content |
| PDF thumbnail in the warm utility session (400×480) | 102–150 ms |
| **Quick Preview of a PDF**, from opening until the page is there | **174 ms** the first time; the helper alone, 125–150 ms |
| Quick Preview of text, code and images | under 1 ms (doesn't go through the helper) |
| Quick Preview of a font | 24 ms |
| Quick Preview of a `.zip` (launches a helper and lists) | 146–310 ms; browsing with the arrows adds Files' redraw (~400 ms) |
| Extract a 100 MB `.tar.gz` (232 MB, 8,958 files) | 1.59 s (GNU tar: 2.32 s); identical content, except an absolute link that was skipped on purpose |
| Save a 1.6 MB annotated copy | 34 ms |
| lord-server RAM | 53.6 MB without documents, 82 MB with the 224-page PDF open at 135 % |
| Helpers' RAM | pdf session: 87 MB with that PDF; utility session: 25 MB |

**The 150 ms goal for Quick Preview:**
- **It's met** with text, images and fonts; a small `.zip`, sometimes.
- **A PDF lands at ~175 ms** on this VM: almost all of it is Poppler's drawing. The utility session
  is already prepared (Files "warms" it when it opens, with a minimal PDF), so the helper's startup
  isn't paid. On a current desktop CPU it should come in below.

The measurements show up in the session log:
- `js: pdf: … first page in N ms (N pages)`;
- `js: quick-look: <kind> <name> in N ms` (Quick Preview, from Files);
- with the file `~/.cache/lordos/doc-trace` (or `LORD_DOC_TRACE=1`), `lord-doc: request N: queue X
  ms, helper Y ms` for each request.

## Tests

```sh
make -C server test        # includes build/arcpath-test, build/ipp-test and build/doc-test
server/build/arcpath-test  # 37/37: names and link targets
sh tests/doc_evil.sh       # 31/31 (in server/): malicious archives, formats, confinement and annotated copy
server/build/doc-test server/build/lord-doc pdf file.pdf 900   # open, draw and measure
```

## Pending

- **Files:**
  - PDF thumbnails in the icon view (today only in Quick Preview);
  - extracting encrypted archives (asking for the password);
  - writing `.tar.gz` or `.7z`.
- **PDF Viewer:**
  - digital signatures and the panel of existing annotations with their text;
  - opening external links in a browser;
  - with the user's rotation, pages that carry their own rotation (`/Rotate`) may have search and
    links shifted (not tested).
- **Scripts:** the text that gets printed (Editor, Notes) is built with per-character fallback,
  without complex-script shaping: Arabic and Indic scripts come out unjoined.
- **Emoji:**
  - ZWJ sequences (❤️‍🔥, 👩‍💻) show as their parts, because the text engine doesn't join them yet;
  - in Linux apps it types by pasting, because there's no Wayland `text-input`.
- **Untested:**
  - Quick Preview of audio and video with `media.poster` was not measured here (VM 116 had no lord-media
    when this was written; `media.poster` exists now, `MEDIA.md`);
  - printing from Photos (it belongs to the media agent; the API is ready: `print.open({ image })`).
