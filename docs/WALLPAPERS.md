# Wallpapers

Settings → Wallpaper is a gallery (0.8.9.6.2, "The gallery" below) of four kinds of wallpaper: LordOS's built-in pictures
(`assets/wallpapers/index.json`), the catalog's still pictures (Abstract, Epic, Anime; downloaded when chosen), the moving
wallpapers (most of this document) and your own pictures.

# Moving wallpapers

Short loops behind the desktop and the login screen, chosen in Settings → Wallpaper. They exist only with the hardware for
them: there is no software mode.

## The rule

A moving wallpaper plays only when **both** hold:

- the GPU composes the session (`lord.media.composition()` is `gpu`);
- the hardware decodes the file's format (`lord.media.hwDecode()`).

If either is missing, the section is disabled with one line, "Moving wallpapers need a compatible GPU.", and a chosen
wallpaper shows its still poster. H.264 is used only where it is decoded in hardware. The login screen follows the same
rule and otherwise shows its still image. The decisions are in `sdk/motionmodel.js`, tested without a screen by
`server/tests/motion_model_test.mjs`.

A virtual machine's GPU never qualifies (`hwDecode().virtualGpu`). On a V4L2 decoder HEVC counts only if the FFmpeg in use
can drive it (`drmprime` from `lm_hwprobe.c`): Debian's FFmpeg can't, Raspberry Pi's can.

## One wallpaper per display

Every theme (0.8.8.10). `settings.wallpaper` is the picture of every display; `settings.wallpapers`
(`{ [connector]: id }`) gives a display its own. The key is the connector (DP-2, HDMI-A-1…), never the model's
name, which three monitors of one model share. The rules are in `sdk/wallpapermodel.js` (`wallpaperIdFor`,
`wallpaperPatch`, `displayLabel`; tests in `server/tests/wallpaper_model_test.mjs`):

- **Settings → Wallpaper** shows a display selector with several displays: *All displays* or one of them (its model,
  plus its connector when two share the model). A picture for one display also turns that display's moving wallpaper
  off (`settings.motion[displayId] = null`, which wins over the one for all displays); a picture for all clears the
  per-display pictures and the moving wallpapers, as before.
- **The Control Center's quick pictures** (Lord Flow, Lord Workspaces) change the display the Control Center opened
  on when there are several.
- Removing one of your own pictures sends a display that showed it back to the picture of every display, and the
  picture of every display back to the theme's default.
- The login screen keeps one picture.
- **Never chosen** (0.8.9): `settings.wallpaper` is `null` and the theme's default shows: Lumo Celestial in Lord Flow
  and Lord Classic, Cyberpunk City HD's panorama in Lord Workspaces (settings migration 5 turned the saved Celestial
  default into `null`; docs/WORKSPACES.md, "Wallpaper and slideshow").
- **A slideshow** of a category's still pictures, with a different one on each display if wanted, is Lord Workspaces'
  only (docs/WORKSPACES.md, "Wallpaper and slideshow"); a moving wallpaper chosen turns it off, and turning it on
  turns the moving wallpapers off.

## Format

HEVC first: it is what a Raspberry Pi 5 decodes in hardware. Each item has `1080.hevc`, `2160.hevc` when the source is 4K,
and `1080.h264` for machines that decode only that. `pickVariant` takes HEVC before H.264, then the smallest file that
covers the display.

Clips last 6 to 10 s. The loop is closed by fading the second after the clip's end over its first second. Nothing is
upscaled or interpolated: frame rate and size never exceed the source's. Of the sources chosen, four are real 60 fps and
the rest 12.5 to 30 fps. No 120, 144 or 240 fps source with a clear license was found.

## When it pauses

`sdk/motion.jsx` pauses the decoder, and `pauseReason` says why: Reduce motion, a fullscreen window, every display showing
it covered by a window, five minutes with no input, the battery at 20 % or less and discharging, or the CPU above 85 % for
two readings 5 s apart. The login screen pauses after five minutes with no input.

One video per file, however many displays show it. The still wallpaper stays drawn underneath, so a failure, a missing
file or a helper crash just shows it.

## Files and downloads

The catalog `assets/wallpapers/motion.json` ships in the package: ids, credits, sizes and a SHA-256 for every file. Its
thumbnails and posters ship next to it (`assets/wallpapers/motion/`, 6.6 MB with the built-in clip).

Videos are downloaded on demand by closed admin actions (`server/src/hw/wallpaper_admin.c`):

| Action | Who | What |
|---|---|---|
| `wallpaper-get "<id> <variant>"` | anyone | downloads a catalog file to `/var/lib/lordos/wallpapers/<sha256>.mp4` (codec `hevc`, `h264`) or `<sha256>.jpg` (codec `jpeg`, a still picture) |
| `wallpaper-remove "<id> <variant>"` | admin | deletes it, either kind |
| `wallpaper-login "<id> <variant>"` or `default` | admin | the login screen plays it (`login.mp4`; a video variant only) or the built-in one |
| `wallpaper-login "still <id>"` or `"still cat:<id>"` | admin | the login screen shows that built-in or catalog picture, with no video (`login-still`) |

JS names an entry, never a URL or a path. The base must be the project's GitHub owner on `raw.githubusercontent.com`, the
bytes go through `fetch_batch` (a child as `nobody`, in the Store's scope, so the firewall sees `lordos:store`), and a file
whose size or hash isn't the catalog's is deleted. The extension a file is kept with comes from the variant's `codec`, read
as root (`hw_wallpaper_find`): `hevc` and `h264` are `.mp4` and their `file` must end in `.mp4`, `jpeg` is `.jpg` and its
`file` must end in `.jpg`; any other codec is no entry. The `file` must be a relative `[a-z0-9._/-]` name with no `..`,
`//` or leading dot or slash, and the `size` a whole number of bytes, at most 100 MB. `lord-media` keeps its sandbox and receives an open file, and
`media_open_input` accepts those two system folders besides the user's own files. `server/tests/wallpaper_find_test.c` covers the lookup, and
`server/tests/wallpaper_admin_test.c` the actions themselves with a fake download (a picture kept as `.jpg`, a video as
`.mp4`, refused arguments never fetch, a wrong hash or a 404 never kept, remove, `still cat:<id>`, every entry of the
shipped catalog accepted).

An added video stays in the user's files. It needs at most 10 s and a format this machine decodes in hardware.

## The login screen

The greeter can't read a user's settings, so the choice reaches it as a file: `/var/lib/lordos/wallpapers/login.mp4`, else
the built-in `crown.hevc.mp4` or `crown.h264.mp4`.

Before 0.8.7 the login screen composed on the CPU: `lordos-greeter` couldn't open `/dev/dri/renderD128`. It now joins
`render` and `video` (`install.sh`, the ISO, the Pi image, migration 006). That gives the login screen's process the GPU
driver's attack surface; it already ran Mesa in software.

## The Raspberry Pi 5

Hardware HEVC needs Raspberry Pi's FFmpeg, pinned in `/etc/apt/preferences.d/raspberrypi` (migration 006 adds the pin on
upgrades, and the next `apt` update brings the packages). That FFmpeg opens the decoder by path, so for this one job,
`MK_PLAY` with `hwaccel=v4l2request`, `lord-media` may open two exact device files found from sysfs before the sandbox,
reads `/sys/class`, `/sys/bus` and `/run/udev`, and gets the V4L2 and media controller ioctl classes.

## Measurements

| Machine | Case | Result |
|---|---|---|
| Pi 5, login, trial build | 1080p60 H.264, software decode, CPU composition | about 156 % of one core |
| Pi 5, login, 0.8.7 | 1080p60 HEVC, hardware decode, GPU composition | `lord-media` 41 %, `lord-server` 147 % of one core, 30 s, SoC 52 °C with its case |

Decoding is in hardware; each frame is then read back and converted to BGRA in `lord-media`, and `lord-server` paints it. That copy is why the total stays high. Showing the decoder's buffer directly on the GPU is the next step, planned for a later release.

Settings → Wallpaper has a switch at the top, Desktop or Login screen, and the same tiles choose for that target: a video (`wallpaper-login "<id> <variant>"`) or a still wallpaper (`wallpaper-login "still <id>"`, kept in `/var/lib/lordos/wallpapers/login-still`, shown with no video at all).

## Your own pictures

Settings → Wallpaper → Your pictures takes a PNG or JPEG, for the desktop or the login screen. Nothing shows the file itself: it is converted once to a baseline JPEG of at most 3840 pixels on its longest side, upright (EXIF orientation applied) and with no metadata, so the session and the login screen load something of a known size. GPS positions and camera details don't reach the login screen. The limits are 64 MB and 100 megapixels, read from the header before any memory is taken. WebP, GIF and BMP are refused.

| Action | Who | What |
|---|---|---|
| `wallpaper-image "<path>"` | anyone | makes the user's copy, `~/.local/share/lordos/wallpapers/<name>.jpg` and `.thumb.jpg`, and answers its name, 16 hex digits |
| `wallpaper-image "remove <name>"` | anyone | deletes that copy |
| `wallpaper-login-image "<name>"` | admin | the login screen shows that copy: `/var/lib/lordos/wallpapers/login-<name>.jpg`, with `image:<name>` in `login-still` |

Root never opens the user's file and never decodes it. A child gives up root for good (`setresuid`, no new privileges, a CPU limit, 30 s from the parent) and closes every file of the helper before it reads anything, so a link to a file only root can read fails, as it would for the user. The file must be a regular file and the last step of its path can't be a link (`O_NOFOLLOW`, and `O_NONBLOCK` so a pipe can't hang it). `wallpaper-login-image` takes a name, not a path; the child reconverts the copy and sends the bytes through a pipe, and root only checks that they are a finished JPEG of an allowed size before writing them to a root-owned file, readable by all, with a name that follows its content. So the greeter, a separate user with no access to any home, only reads a system folder, and a new choice is never hidden by an image cached under the old name. Every other choice for the login screen deletes the stored picture.

A file that goes missing costs nothing: the desktop goes back to the default wallpaper, the login screen to its usual background. The remote login page keeps its built-in background. `server/tests/wallpaper_image_test.c` covers the conversion and the refusals.

Settings offers 1×, 0.75×, 0.5× and 0.25×. A slower speed decodes fewer frames per second, and it applies to the desktop, not to the login screen.

Choosing a still wallpaper turns the moving one off, since it would stay in front. A new player can start while the last one still holds the decoder, so a stop nobody asked for is retried four times, half a second apart; the tile shows a spinner until the video plays.

A moving wallpaper plays with `hwOnly`: if the hardware decoder doesn't start, or fails, `lord-media` stops rather than decoding in software, and the still image stays.

## The collection

Files live in a separate repository, `LordOS-wallpapers`, because 353 MB of video doesn't belong in the code's history. Its
README credits each clip; licenses come from each file's own page. Only public domain, CC0, CC BY and CC BY-SA sources are
used.

# The gallery and the still pictures (0.8.9.6.2)

## The catalog's still pictures

The catalog (`assets/wallpapers/motion.json`, a copy of the `LordOS-wallpapers` repository's `catalog.json`) keeps its
structure. A still picture is an item like a moving one, in the section `abstract`, `epic` or `anime`, whose variants have
codec `jpeg`, `fps` and `seconds` 0:

```json
{ "name": "1080.jpeg", "codec": "jpeg", "w": 1920, "h": 1080, "fps": 0.0, "seconds": 0.0,
  "file": "stills/<section>/<id>/1080.jpg", "size": 437074, "sha256": "…" }
```

and the same with `2160.jpeg`, 3840 × 2160. Its 320 × 180 thumbnail ships as `assets/wallpapers/motion/<id>.thumb.jpg`,
next to the moving wallpapers'. 0.8.9.6.2 ships the 50 Epic pictures; nothing in the code depends on how many there
are. The pictures are made with AI for LordOS and released as CC0. `sdk/motion.jsx` plays only the video items
(`motionCatalog`): a `jpeg` variant is never a moving wallpaper.

## How a chosen picture is saved and drawn

A catalog picture is saved as **`cat:<catalog id>`** wherever a wallpaper id goes: `settings.wallpaper` (every display),
`settings.wallpapers[connector]` (one display), `settings.loginWallpaper` and `login-still` (the login screen,
`wallpaper-login "still cat:<id>"`), and a slideshow's category is the collection's section. Older settings keep working
as they were: no migration.

`findWallpaper` (sdk/wallpapers.js) resolves `cat:<id>` after the theme's own list and your pictures: it lists
`/var/lib/lordos/wallpapers` (at most every 10 s, or at once after a download or a removal in this session) and offers
the variants whose `<sha256>.jpg` is there at the catalog's size. Every theme (Lord Flow, Lord Classic, Lord Workspaces,
Celeste), the greeter and the slideshow already go through `findWallpaper` and `wallpaperSrc`, so each display draws the
smallest file that covers it. A picture whose files are gone (removed, or settings brought from another computer)
gives `null`, and the theme's default shows (the login screen: its own).

**Quality** (`settings.wallpaperQuality`): `auto` (the default), `hd` or `4k`. Auto takes the 4K file only for a display
wider than 1920 or taller than 1080 device pixels (its scale applied) and the HD file for the others, display by display;
choosing a picture downloads what the displays it goes on need. HD and 4K offer only that file when it is on this
computer, and otherwise what is. The rules are in `sdk/wallpapergallery.js` (`autoQuality`, `variantsToGet`,
`drawnVariant`, `stillWallpaper`), tested by `server/tests/wallpaper_gallery_test.mjs`.

## Built-in pictures with a catalog twin

A catalog still item whose id is a built-in wallpaper's (`assets/wallpapers/index.json`, e.g. Cyberpunk City HD's
`cyberpunk-panorama`, section = the built-in's category) is that built-in's 4K, never a picture of its own: `stillItems`
(the `cat:<id>` pictures) leaves it out and `twinItems` holds it. There is no special case for a category in the code.

- **One tile** in the built-in collection: ✓ (the HD file is built in), the 4K badge; the sheet lists the built-in HD
  ("included, works offline") and the twin's 4K with its size; the collection's page offers Download all · 4K.
- **Settings keep the plain built-in id.** Choosing it where a display wants 4K (Auto above 1920 × 1080, or 4K) fetches
  the twin's largest variant first (`wallpaper-get "<id> 2160.jpeg"`).
- **Drawing**: `findWallpaper` passes a built-in through `withTwinFile` → `withTwin`: Auto adds the twin's 4K file to the
  built-in's variants once it is on disk, so `wallpaperSrc` picks it per display and the built-in HD everywhere else and
  while it isn't there; HD keeps the built-in files only; 4K keeps the 4K file when it is here, else the built-in. A
  built-in that already ships a larger file than the twin's keeps its own (`twinExtra` is null). The same path serves
  every theme, the login screen (`login-still` holds the built-in id) and the slideshow (`withDownloadedStills`).
- **Free up space** may delete a twin's 4K only when nothing of this account shows it: the desktop and every display,
  the login screen, a running slideshow of its collection, or the theme's default while the wallpaper setting is unset.
  The built-in HD is what shows afterwards.

## The page

`apps/settings/WallpaperGallery.jsx`:

- **What shows now**: a large picture with its name, collection and license; For (Desktop / Login screen), Display (All /
  each display, with several) and Quality, with the resolution each display gets.
- **Search and filters**: All, On this computer, Pictures (built-in and catalog pictures), Moving, My photos, each with its
  count. All leaves your pictures to My photos.
- **Collections**: with no search and All, a card per collection (a stack of three thumbnails, its count, NEW on the
  still collections), then one row of each with See all. Order: the still collections, the built-in categories
  (`WALLPAPER_CATEGORIES`, then a theme's own), the moving sections. A collection's page offers Download all in HD or 4K
  (the bytes still to fetch) and Use as slideshow where the theme has one (two pictures or more on this computer).
- **Tiles**: a cloud (downloads when chosen), a check (on this computer), 4K or HD, a play mark for a moving one, the
  download's progress along the bottom edge.
- **A sheet per wallpaper**: a large picture, credits, the quality options with the catalog's sizes, Use on desktop,
  Login screen, Only <display>, and Remove download when this account doesn't use it.
- **The bottom**: the space the catalog's files on this computer take, **Free up space** (deletes the ones this account
  doesn't use: the desktop and every display, the login screen, the moving wallpapers, random's videos and a running
  slideshow's collection stay), the thumbnails' size, the slideshow, the moving wallpapers' options.

The store is shared by every account and only an administrator removes from it (`wallpaper-remove`); another account's
wallpaper whose file is removed falls back to its theme's default. `server/tests/app_scene_headless.sh
settings_wallpaper_scene` drives the page headless and keeps screenshots.
